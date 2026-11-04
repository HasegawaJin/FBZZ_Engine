/// @file    ProjectSettingsOperators.cpp
/// @brief   全プロジェクト設定の照会・部分編集・再読込・初期化・保存要求。
/// @author  Hasegawa Jin
/// @date    2026-11-03
#include "SettingsDocument.hpp"
#include <Editor/EditorContext.hpp>
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/Util/ProjectSettingsEditing.hpp>
#include <Engine/Util/FileSystem.hpp>

namespace fbzz::editor {

void RegisterProjectSettingsOperators(OperatorRegistry& registry)
{
    for (const char* action : {"inspect", "configure", "reset", "reload", "save"}) {
        EditorOperator op;
        const std::string actionName = action;
        op.id = "project.settings." + actionName; op.label = op.id; op.category = "Project Settings";
        op.kind = actionName == "inspect" ? OpKind::Query : actionName == "save" ? OpKind::Action : OpKind::Mutation;
        op.desc = "全プロジェクト設定をsettingsのキーで照会・部分変更する。configure/reset/reloadはUndo対応。saveの完了はinspectのsaveStateで確認する。";
        if (actionName == "configure") {
            OpParam patch; patch.name = "settings"; patch.type = OpParamType::Data;
            patch.desc = "inspectが返すsettingsの部分オブジェクト。配列は全体を置換し、未知キー・型不一致・範囲外は原子的に拒否する。";
            op.params.push_back(std::move(patch));
        }
        op.poll = [](const OpContext& context, const OpArgs&) { return !context.ctx.projectRoot.empty(); };
        op.exec = [actionName](OpContext& context, const OpArgs& args) {
            EditorContext& ctx = context.ctx;
            if (actionName == "save") {
                if (ctx.projectSettingsPath.empty()) return OpResult::Err("NO_SETTINGS_PATH", "プロジェクト設定の保存先がありません。");
                ctx.requestProjectSettingsSave = true;
            } else if (actionName != "inspect") {
                const ProjectSettings before = ctx.projectSettings;
                ProjectSettings after = before;
                if (actionName == "reset") after = ProjectSettings::Default();
                else {
                    std::string text;
                    if (actionName == "reload") {
                        if (!util::FileSystem::ReadText(ctx.projectSettingsPath, text))
                            return OpResult::Err("SETTINGS_READ_FAILED", "プロジェクト設定を読み取れません。");
                    } else {
                        const OpData* patch = args.GetData("settings");
                        if (!patch || !settingsdocument::Patch(before.ToToml(true), *patch, text))
                            return OpResult::Err("BAD_ARG", "settingsに未知のキーまたは不正な型があります。");
                    }
                    if (!after.LoadFromToml(text)) return OpResult::Err("BAD_ARG", "設定内容を解析できません。");
                    if (actionName == "configure" && !settingsdocument::Preserves(after.ToToml(true), *args.GetData("settings")))
                        return OpResult::Err("BAD_ARG", "設定値が許容範囲外、またはローダーが受け付けない値です。");
                }
                OpResult result;
                result.command = CommitProjectSettingsEdit(ctx, before, after);
                result.noChange = !result.command;
                result.data = settingsdocument::Inspect(ctx.projectSettings.ToToml(true));
                return result;
            }
            OpData data = OpData::MakeObject();
            data.Set("path", OpData(ctx.projectSettingsPath));
            data.Set("settings", settingsdocument::Inspect(ctx.projectSettings.ToToml(true)));
            data.Set("saveState", OpData(ctx.requestProjectSettingsSave ? "pending"
                : ctx.projectSettingsSaveState == EditorContext::SettingsSaveState::Failed ? "failed"
                : ctx.projectSettingsSaveState == EditorContext::SettingsSaveState::Pending ? "pending" : "saved"));
            return OpResult::Data(std::move(data));
        };
        registry.Register(std::move(op));
    }
}
} /// @note namespace fbzz::editor
