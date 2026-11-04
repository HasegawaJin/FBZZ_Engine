/// @file    AssetReferenceOperators.cpp
/// @brief   保存済みアセット参照の共通スキャンを直接APIへ公開する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include "../Panels/AssetBrowser/AssetReferenceScan.hpp"
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/AssetSearch.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <algorithm>
#include <filesystem>
#include <memory>

namespace fbzz::editor {
namespace {
struct ReferenceJob { int id = 0; assetrefs::Scan scan; };
struct ReferenceJobs { int nextId = 1; std::vector<std::unique_ptr<ReferenceJob>> jobs; };
OpData Metadata(const assetrefs::ImportMetadata& metadata)
{
    OpData data = OpData::MakeObject(); data.Set("sourceDcc", OpData(metadata.sourceDcc)); data.Set("importer", OpData(metadata.importer));
    data.Set("importerVersion", OpData(metadata.importerVersion)); data.Set("unreadable", OpData(metadata.unreadable)); return data;
}
std::string ResolveTarget(EditorContext& ctx, const std::string& input)
{
    if (input.empty() || ctx.projectRoot.empty()) return {};
    const std::string path = input.starts_with("guid:") ? asset::AssetDatabase::PathFromGuid(asset::AssetDatabase::GuidFromRef(input)) : input;
    if (path.empty()) return {};
    auto absolute = util::FileSystem::PathFromUtf8(path);
    if (absolute.is_relative()) absolute = util::FileSystem::PathFromUtf8(ctx.projectRoot) / absolute;
    std::error_code error; absolute = std::filesystem::weakly_canonical(absolute, error);
    if (error || !std::filesystem::is_regular_file(absolute, error)) return {};
    const auto root = std::filesystem::weakly_canonical(util::FileSystem::PathFromUtf8(ctx.projectRoot) / "Assets", error);
    const auto relative = absolute.lexically_relative(root);
    if (error || relative.empty() || relative.is_absolute() || *relative.begin() == "..") return {};
    return util::FileSystem::PathToUtf8(absolute);
}
} /// @note namespace
void RegisterAssetReferenceOperators(OperatorRegistry& registry)
{
    const auto state = std::make_shared<ReferenceJobs>();
    for (const char* action : {"scan", "status"}) {
        EditorOperator op; const std::string name = action;
        op.id = "asset.references." + name; op.label = op.id; op.category = "Asset References";
        op.kind = name == "scan" ? OpKind::Action : OpKind::Query;
        op.desc = "Find Referencesと同じ保存済みTOMLの直接参照を調べる。scanでjobを受け、statusを繰り返す。未保存・動的/外部/推移参照は対象外。stale/limited/finishedで調査範囲と鮮度を確認する。";
        if (name == "scan") { OpParam path; path.name = "path"; path.type = OpParamType::String; op.params.push_back(std::move(path)); }
        else {
            OpParam job; job.name = "job"; job.type = OpParamType::Int; op.params.push_back(std::move(job));
            for (const char* field : {"maxFiles", "offset", "limit"}) {
                const std::string key = field; OpParam value; value.name = field; value.type = OpParamType::Int; value.required = false;
                value.defaultValue = key == "maxFiles" ? 4 : key == "limit" ? 100 : 0; value.hasRange = true;
                value.minValue = key == "offset" ? 0 : 1; value.maxValue = key == "maxFiles" ? 32 : key == "limit" ? 100 : 1000000;
                op.params.push_back(std::move(value));
            }
        }
        op.exec = [state, name](OpContext& context, const OpArgs& args) {
            auto& ctx = context.ctx; ReferenceJob* job = nullptr;
            if (name == "scan") {
                const std::string path = ResolveTarget(ctx, args.GetString("path"));
                if (path.empty()) return OpResult::Err("BAD_PATH", "Assets配下の参照対象を指定してください。");
                AssetSearch::SetProjectRoot(ctx.projectRoot);
                auto created = std::make_unique<ReferenceJob>(); created->id = state->nextId++;
                created->scan.Begin({ctx.projectRoot, path, AssetSearch::Revision(), asset::AssetDatabase::Revision(), AssetSearch::Entries()});
                if (state->jobs.size() >= 32) state->jobs.erase(state->jobs.begin());
                job = created.get(); state->jobs.push_back(std::move(created));
            } else {
                for (auto& candidate : state->jobs) if (candidate->id == args.GetInt("job")) { job = candidate.get(); break; }
                if (!job) return OpResult::Err("REFERENCE_JOB_NOT_FOUND", "その参照検索ジョブはありません。");
                if (util::FileSystem::SamePathText(ctx.projectRoot, job->scan.Result().projectRoot)) job->scan.Advance(static_cast<std::size_t>(args.GetInt("maxFiles", 4)));
            }
            const auto& report = job->scan.Result();
            const bool stale = !job->scan.MatchesSnapshot(ctx.projectRoot, AssetSearch::Revision(), asset::AssetDatabase::Revision());
            OpData owners = OpData::MakeArray(); const auto offset = static_cast<std::size_t>(args.GetInt("offset", 0));
            const auto end = std::min(report.owners.size(), offset + static_cast<std::size_t>(args.GetInt("limit", 100)));
            for (std::size_t i = offset; i < end; ++i) {
                const auto& owner = report.owners[i]; OpData row = OpData::MakeObject(); row.Set("path", OpData(owner.path)); row.Set("kind", OpData(owner.kind));
                row.Set("metadata", Metadata(owner.metadata)); OpData evidence = OpData::MakeArray();
                for (const auto& entry : owner.evidence) { OpData value = OpData::MakeObject(); value.Set("field", OpData(entry.field));
                    value.Set("kind", OpData(entry.kind)); value.Set("line", OpData(entry.line)); evidence.Push(std::move(value)); }
                row.Set("evidence", std::move(evidence)); owners.Push(std::move(row));
            }
            OpData data = OpData::MakeObject(); data.Set("job", OpData(job->id)); data.Set("target", OpData(report.targetPath));
            data.Set("finished", OpData(report.finished)); data.Set("stale", OpData(stale)); data.Set("limited", OpData(report.limited));
            data.Set("owners", std::move(owners)); data.Set("ownerCount", OpData(static_cast<std::int64_t>(report.owners.size())));
            data.Set("processed", OpData(static_cast<std::int64_t>(report.processed))); data.Set("candidates", OpData(static_cast<std::int64_t>(report.candidates)));
            data.Set("unreadable", OpData(static_cast<std::int64_t>(report.unreadable))); data.Set("unsupported", OpData(static_cast<std::int64_t>(report.unsupported)));
            data.Set("oversized", OpData(static_cast<std::int64_t>(report.oversized))); data.Set("malformed", OpData(static_cast<std::int64_t>(report.malformed)));
            data.Set("unresolvedHints", OpData(static_cast<std::int64_t>(report.unresolvedHints))); data.Set("metadata", Metadata(report.metadata));
            return OpResult::Data(std::move(data));
        }; registry.Register(std::move(op));
    }
}
} /// @note namespace fbzz::editor
