/// @file    AssetReferenceOperators.cpp
/// @brief   保存済みアセット参照の共通スキャンを直接APIへ公開する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include "../../Panels/AssetBrowser/AssetReferenceScan.hpp"
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Asset/Browser/AssetSearch.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Util/Files/FileSystem.hpp>
#include <algorithm>
#include <filesystem>
#include <memory>

namespace fbzz::editor {
namespace {
struct ReferenceJob { int id = 0; assetrefs::Scan scan; };
struct ReferenceJobs { int nextId = 1; std::vector<std::unique_ptr<ReferenceJob>> jobs; };
struct GraphJob { int id = 0; assetrefs::GraphScan scan; };
struct GraphJobs { int nextId = 1; std::vector<std::unique_ptr<GraphJob>> jobs; };
OpData Metadata(const assetrefs::ImportMetadata& metadata)
{
    OpData data = OpData::MakeObject();
    data.Set("sourceDcc", OpData(metadata.sourceDcc));
    data.Set("importer", OpData(metadata.importer));
    data.Set("importerVersion", OpData(metadata.importerVersion));
    data.Set("unreadable", OpData(metadata.unreadable));
    return data;
}
std::string ResolveTarget(EditorContext& ctx, const std::string& input)
{
    if (input.empty() || ctx.projectRoot.empty()) {
        return {};
    }
    const std::string path = input.starts_with("guid:") ? asset::AssetDatabase::PathFromGuid(asset::AssetDatabase::GuidFromRef(input)) : input;
    if (path.empty()) {
        return {};
    }
    auto absolute = util::FileSystem::PathFromUtf8(path);
    if (absolute.is_relative()) {
        absolute = util::FileSystem::PathFromUtf8(ctx.projectRoot) / absolute;
    }
    std::error_code error; absolute = std::filesystem::weakly_canonical(absolute, error);
    if (error || !std::filesystem::is_regular_file(absolute, error)) {
        return {};
    }
    const auto root = std::filesystem::weakly_canonical(util::FileSystem::PathFromUtf8(ctx.projectRoot) / "Assets", error);
    const auto relative = absolute.lexically_relative(root);
    if (error || relative.empty() || relative.is_absolute() || *relative.begin() == "..") {
        return {};
    }
    return util::FileSystem::PathToUtf8(absolute);
}

OpData Count(std::size_t value)
{
    return OpData(static_cast<std::int64_t>(value));
}

OpData DescribeNode(const assetrefs::GraphNode& node)
{
    const char* state = "unverified";
    switch (node.state) {
    case assetrefs::GraphNodeState::RESOLVED: state = "resolved"; break;
    case assetrefs::GraphNodeState::MISSING_GUID: state = "missingGuid"; break;
    case assetrefs::GraphNodeState::MISSING_PATH: state = "missingPath"; break;
    case assetrefs::GraphNodeState::UNVERIFIED: break;
    }
    OpData data = OpData::MakeObject();
    data.Set("guid", node.guid);
    data.Set("path", node.path);
    data.Set("kind", node.kind);
    data.Set("state", state);
    return data;
}

OpData DescribeGraph(const GraphJob& job, bool stale, bool incoming, std::size_t offset, std::size_t limit)
{
    const auto& report = job.scan.Result();
    const auto& direction = incoming ? report.incoming : report.outgoing;
    OpData edges = OpData::MakeArray();
    if (!stale) {
        const auto end = std::min(direction.hops.size(), offset + limit);
        for (std::size_t index = offset; index < end; ++index) {
            const auto& hop = direction.hops[index];
            const auto& link = report.edges[hop.edge];
            OpData row = OpData::MakeObject();
            row.Set("source", DescribeNode(report.nodes[link.source]));
            row.Set("target", DescribeNode(report.nodes[link.target]));
            row.Set("distance", Count(hop.distance));
            row.Set("field", link.field);
            row.Set("line", link.line);
            row.Set("referenceKind", link.kind);
            row.Set("suffix", link.suffix);
            row.Set("hint", link.hint);
            edges.Push(std::move(row));
        }
    }
    OpData coverage = OpData::MakeObject();
    coverage.Set("candidates", Count(report.candidates));
    coverage.Set("processed", Count(report.processed));
    coverage.Set("parsed", Count(report.parsed));
    coverage.Set("unsupported", Count(report.unsupported));
    coverage.Set("unreadable", Count(report.unreadable));
    coverage.Set("oversized", Count(report.oversized));
    coverage.Set("malformed", Count(report.malformed));
    OpData data = OpData::MakeObject();
    data.Set("job", job.id);
    data.Set("root", DescribeNode(report.nodes[report.root]));
    data.Set("direction", incoming ? "incoming" : "outgoing");
    data.Set("scope", "indexed-saved-assets");
    data.Set("finished", report.finished);
    data.Set("stale", stale);
    data.Set("limited", report.limited);
    data.Set("incomplete", !report.finished || stale || report.limited || report.unreadable != 0 || report.malformed != 0);
    data.Set("coverage", std::move(coverage));
    data.Set("maxDepth", Count(report.maxDepth));
    data.Set("depthLimited", direction.depthLimited);
    data.Set("totalEdges", Count(direction.hops.size()));
    data.Set("relatedCount", Count(direction.relatedCount));
    data.Set("brokenCount", Count(direction.brokenCount));
    data.Set("unverifiedCount", Count(direction.unverifiedCount));
    data.Set("edges", std::move(edges));
    return data;
}

bool GraphStale(const GraphJob& job)
{
    const auto& snapshot = job.scan.Result();
    return snapshot.searchRevision != AssetSearch::Revision()
        || snapshot.databaseRevision != asset::AssetDatabase::Revision();
}

OpParam IntegerOption(std::string name, int defaultValue, float minValue, float maxValue)
{
    OpParam value;
    value.name = std::move(name);
    value.type = OpParamType::Int;
    value.required = false;
    value.defaultValue = defaultValue;
    value.hasRange = true;
    value.minValue = minValue;
    value.maxValue = maxValue;
    return value;
}

void RegisterAssetDependencyGraph(OperatorRegistry& registry)
{
    const auto state = std::make_shared<GraphJobs>();
    EditorOperator scan;
    scan.id = "asset.dependencies.scan";
    scan.label = "Scan Asset Dependencies";
    scan.category = "Asset References";
    scan.desc = "Start a saved-file dependency graph at an asset GUID. Poll status and page outgoing or incoming edges.";
    scan.kind = OpKind::Query;
    scan.dialog = OpDialog::NONE;
    OpParam guid;
    guid.name = "guid";
    guid.type = OpParamType::String;
    guid.desc = "32 hexadecimal digits, optionally prefixed by guid:. Missing GUIDs can be used to find their saved owners.";
    scan.params = {std::move(guid), IntegerOption("maxDepth", 4, 1, 16)};
    scan.exec = [state](OpContext& context, const OpArgs& args) {
        auto& ctx = context.ctx;
        if (ctx.projectRoot.empty()) {
            return OpResult::Err("NO_PROJECT", "プロジェクトを開いてください。");
        }
        const auto root = assetrefs::NormalizeGraphGuid(args.GetString("guid"));
        if (root.empty()) {
            return OpResult::Err("BAD_GUID", "guid は32桁の16進数です。");
        }
        AssetSearch::SetProjectRoot(ctx.projectRoot);
        auto job = std::make_unique<GraphJob>();
        job->id = state->nextId++;
        job->scan.Begin({ctx.projectRoot, root, AssetSearch::Revision(), asset::AssetDatabase::Revision(), AssetSearch::Entries()},
            static_cast<std::size_t>(args.GetInt("maxDepth")));
        auto result = OpResult::Data(DescribeGraph(*job, GraphStale(*job), false, 0, 0));
        if (state->jobs.size() == 4) {
            state->jobs.erase(state->jobs.begin());
        }
        state->jobs.push_back(std::move(job));
        return result;
    };
    registry.Register(std::move(scan));

    EditorOperator status;
    status.id = "asset.dependencies.status";
    status.label = "Read Asset Dependencies";
    status.category = "Asset References";
    status.desc = "Advance a bounded saved-file scan or page the completed graph. Incoming finds impact, outgoing finds dependencies and missing targets.";
    status.kind = OpKind::Query;
    status.dialog = OpDialog::NONE;
    OpParam job;
    job.name = "job";
    job.type = OpParamType::Int;
    OpParam direction;
    direction.name = "direction";
    direction.type = OpParamType::String;
    direction.enumValues = {"incoming", "outgoing"};
    direction.required = false;
    direction.defaultValue = std::string("outgoing");
    status.params = {std::move(job), std::move(direction), IntegerOption("offset", 0, 0, 4096),
        IntegerOption("limit", 100, 1, 100), IntegerOption("maxFiles", 4, 1, 32)};
    status.exec = [state](OpContext& context, const OpArgs& args) {
        const auto id = args.GetInt("job");
        const auto found = std::find_if(state->jobs.begin(), state->jobs.end(), [id](const auto& candidate) { return candidate->id == id; });
        if (found == state->jobs.end() ||
            !util::FileSystem::SamePathText(context.ctx.projectRoot, (*found)->scan.Result().projectRoot)) {
            return OpResult::Err("REFERENCE_JOB_NOT_FOUND", "現在のプロジェクトで新しい参照調査を開始してください。");
        }
        auto& current = **found;
        if (!GraphStale(current)) {
            current.scan.Advance(static_cast<std::size_t>(args.GetInt("maxFiles")));
        }
        return OpResult::Data(DescribeGraph(current, GraphStale(current), args.GetString("direction") == "incoming",
            static_cast<std::size_t>(args.GetInt("offset")), static_cast<std::size_t>(args.GetInt("limit"))));
    };
    registry.Register(std::move(status));
}
} // namespace
void RegisterAssetReferenceOperators(OperatorRegistry& registry)
{
    RegisterAssetDependencyGraph(registry);
    const auto state = std::make_shared<ReferenceJobs>();
    for (const char* action : {"scan", "status"}) {
        EditorOperator op;
        const std::string name = action;
        op.id = "asset.references." + name;
        op.label = op.id;
        op.category = "Asset References";
        op.kind = name == "scan" ? OpKind::Action : OpKind::Query;
        op.desc = "Find Referencesと同じ保存済みTOMLの直接参照を調べる。scanでjobを受け、statusを繰り返す。未保存・動的/外部/推移参照は対象外。stale/limited/finishedで調査範囲と鮮度を確認する。";
        if (name == "scan") {
            OpParam path;
            path.name = "path";
            path.type = OpParamType::String;
            op.params.push_back(std::move(path));
        } else {
            OpParam job;
            job.name = "job";
            job.type = OpParamType::Int;
            op.params.push_back(std::move(job));
            for (const char* field : {"maxFiles", "offset", "limit"}) {
                const std::string key = field;
                OpParam value;
                value.name = field;
                value.type = OpParamType::Int;
                value.required = false;
                value.defaultValue = key == "maxFiles" ? 4 : key == "limit" ? 100 : 0;
                value.hasRange = true;
                value.minValue = key == "offset" ? 0 : 1;
                value.maxValue = key == "maxFiles" ? 32 : key == "limit" ? 100 : 1000000;
                op.params.push_back(std::move(value));
            }
        }
        op.exec = [state, name](OpContext& context, const OpArgs& args) {
            auto& ctx = context.ctx;
            ReferenceJob* job = nullptr;
            if (name == "scan") {
                const std::string path = ResolveTarget(ctx, args.GetString("path"));
                if (path.empty()) {
                    return OpResult::Err("BAD_PATH", "Assets配下の参照対象を指定してください。");
                }
                AssetSearch::SetProjectRoot(ctx.projectRoot);
                auto created = std::make_unique<ReferenceJob>();
                created->id = state->nextId++;
                created->scan.Begin({ctx.projectRoot, path, AssetSearch::Revision(), asset::AssetDatabase::Revision(), AssetSearch::Entries()});
                if (state->jobs.size() >= 32) {
                    state->jobs.erase(state->jobs.begin());
                }
                job = created.get();
                state->jobs.push_back(std::move(created));
            } else {
                for (auto& candidate : state->jobs) if (candidate->id == args.GetInt("job")) { job = candidate.get(); break; }
                if (!job) {
                    return OpResult::Err("REFERENCE_JOB_NOT_FOUND", "その参照検索ジョブはありません。");
                }
                if (util::FileSystem::SamePathText(ctx.projectRoot, job->scan.Result().projectRoot)) {
                    job->scan.Advance(static_cast<std::size_t>(args.GetInt("maxFiles", 4)));
                }
            }
            const auto& report = job->scan.Result();
            const bool stale = !job->scan.MatchesSnapshot(ctx.projectRoot, AssetSearch::Revision(), asset::AssetDatabase::Revision());
            OpData owners = OpData::MakeArray();
            const auto offset = static_cast<std::size_t>(args.GetInt("offset", 0));
            const auto end = std::min(report.owners.size(), offset + static_cast<std::size_t>(args.GetInt("limit", 100)));
            for (std::size_t i = offset; i < end; ++i) {
                const auto& owner = report.owners[i];
                OpData row = OpData::MakeObject();
                row.Set("path", OpData(owner.path));
                row.Set("kind", OpData(owner.kind));
                row.Set("metadata", Metadata(owner.metadata));
                OpData evidence = OpData::MakeArray();
                for (const auto& entry : owner.evidence) { OpData value = OpData::MakeObject(); value.Set("field", OpData(entry.field));
                    value.Set("kind", OpData(entry.kind));
                    value.Set("line", OpData(entry.line));
                    evidence.Push(std::move(value)); }
                row.Set("evidence", std::move(evidence));
                owners.Push(std::move(row));
            }
            OpData data = OpData::MakeObject();
            data.Set("job", OpData(job->id));
            data.Set("target", OpData(report.targetPath));
            data.Set("finished", OpData(report.finished));
            data.Set("stale", OpData(stale));
            data.Set("limited", OpData(report.limited));
            data.Set("owners", std::move(owners));
            data.Set("ownerCount", OpData(static_cast<std::int64_t>(report.owners.size())));
            data.Set("processed", OpData(static_cast<std::int64_t>(report.processed)));
            data.Set("candidates", OpData(static_cast<std::int64_t>(report.candidates)));
            data.Set("unreadable", OpData(static_cast<std::int64_t>(report.unreadable)));
            data.Set("unsupported", OpData(static_cast<std::int64_t>(report.unsupported)));
            data.Set("oversized", OpData(static_cast<std::int64_t>(report.oversized)));
            data.Set("malformed", OpData(static_cast<std::int64_t>(report.malformed)));
            data.Set("unresolvedHints", OpData(static_cast<std::int64_t>(report.unresolvedHints)));
            data.Set("metadata", Metadata(report.metadata));
            return OpResult::Data(std::move(data));
        }; registry.Register(std::move(op));
    }
}
} // namespace fbzz::editor
