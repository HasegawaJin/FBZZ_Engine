/// @file    AssetMaintenanceOperators.cpp
/// @brief   GUID衝突の照会と同一修復規則を直接APIへ公開する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <Editor/Panels/AssetMaintenancePanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/PlayModeController.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <filesystem>

namespace fbzz::editor {
void AssetMaintenancePanel::OnRegisterOperators(OperatorRegistry& registry)
{
    for (const char* action : {"inspect", "repair"}) {
        EditorOperator op; const std::string name = action;
        op.id = "asset.guid." + name; op.label = op.id; op.category = "Asset Maintenance";
        op.kind = name == "inspect" ? OpKind::Query : OpKind::Action;
        op.desc = "GUID衝突をグループで照会し、最古metaを保持するUIと同じ規則で修復する。repairのguidはinspectのguidまたはall。生成物とimport fingerprintも移す。";
        if (name == "repair") { OpParam guid; guid.name = "guid"; guid.type = OpParamType::String; op.params.push_back(std::move(guid)); op.caution = "再発行したGUIDはUndoできない。inspectが各ファイルのkeep/reassignを返す。"; }
        op.poll = [name](const OpContext& context, const OpArgs&) { return !context.ctx.projectRoot.empty() && (name == "inspect" || !context.ctx.playMode || context.ctx.playMode->IsInEditor()); };
        op.exec = [this, name](OpContext& context, const OpArgs& args) {
            Rescan(); int fixed = 0, expected = 0; bool found = false;
            if (name == "repair") {
                if (context.ctx.modelImportBusy && context.ctx.modelImportBusy()) return OpResult::Err("MODEL_IMPORT_BUSY", "変換完了後に修復してください。");
                const std::string guid = args.GetString("guid");
                std::error_code error;
                const auto root = std::filesystem::weakly_canonical(util::FileSystem::PathFromUtf8(context.ctx.projectRoot) / "Assets", error);
                if (error) return OpResult::Err("BAD_PATH", "Assetsのルートを解決できません。");
                for (const auto& group : m_groups) {
                    if (guid != "all" && group.guid != guid) continue;
                    for (std::size_t i = 1; i < group.entries.size(); ++i) {
                        const auto path = std::filesystem::weakly_canonical(util::FileSystem::PathFromUtf8(group.entries[i].path), error);
                        const auto relative = path.lexically_relative(root);
                        if (error || relative.empty() || relative.is_absolute() || *relative.begin() == "..")
                            return OpResult::Err("READ_ONLY_ASSET", "共有SDKのアセットは再発行できません。");
                    }
                }
                for (const auto& group : m_groups) {
                    if (guid != "all" && group.guid != guid) continue;
                    found = true; for (std::size_t i = 1; i < group.entries.size(); ++i) if (group.entries[i].exists) ++expected;
                    fixed += FixGroup(group);
                }
                if (!found && guid != "all") return OpResult::Err("NO_CONFLICT", "そのGUIDの衝突はありません。");
                Rescan(); context.ctx.requestAssetBrowserRefresh = true;
            }
            OpData groups = OpData::MakeArray();
            for (const auto& group : m_groups) {
                OpData row = OpData::MakeObject(); row.Set("guid", OpData(group.guid)); OpData entries = OpData::MakeArray();
                for (std::size_t i = 0; i < group.entries.size(); ++i) {
                    const auto& entry = group.entries[i]; OpData value = OpData::MakeObject(); value.Set("path", OpData(entry.path));
                    value.Set("relative", OpData(entry.relative)); value.Set("modified", OpData(entry.metaTime)); value.Set("exists", OpData(entry.exists));
                    value.Set("action", OpData(i == 0 ? "keep" : "reassign")); entries.Push(std::move(value));
                }
                row.Set("entries", std::move(entries)); groups.Push(std::move(row));
            }
            OpData data = OpData::MakeObject(); data.Set("groups", std::move(groups)); data.Set("fixed", OpData(fixed));
            data.Set("failed", OpData(expected - fixed)); return OpResult::Data(std::move(data));
        }; registry.Register(std::move(op));
    }
}
} /// @note namespace fbzz::editor
