/// @file    ScriptRequirementOperators.cpp
/// @brief   必須設定の検証と対象選択を共通 Operator として提供する。
/// @author  Hasegawa Jin
/// @date    2026-09-29
#include <Editor/Util/ScriptRequirementReview.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/Selection.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <limits>

namespace fbzz::editor {
bool CanReviewScriptRequirements(const EditorContext& ctx)
{
    return ctx.activeScene && !ctx.scriptReloadBusy &&
        (!ctx.playMode || ctx.playMode->IsInEditor());
}

bool IsScriptRequirementReviewCurrent(const EditorContext& ctx)
{
    const auto& review = ctx.scriptRequirementReview;
    return review.checked && review.checkedScene == ctx.activeScene &&
        review.scenePath == ctx.currentScenePath;
}

void RefreshScriptRequirementReview(EditorContext& ctx)
{
    if (!CanReviewScriptRequirements(ctx)) return;
    auto& review = ctx.scriptRequirementReview;
    review.rows.clear();
    review.focus = {};
    review.message.clear();
    review.scriptsChecked = 0;
    review.unavailableScripts = 0;
    review.checkedScene = ctx.activeScene;
    review.scenePath = ctx.currentScenePath;
    review.checked = true;
    review.revision = review.revision == std::numeric_limits<int>::max() ? 1 : review.revision + 1;
    for (const auto id : ctx.activeScene->GetEntities<scene::ScriptComponent>()) {
        auto* go = ctx.activeScene->GetGameObject(id);
        auto* component = ctx.activeScene->GetComponent<scene::ScriptComponent>(id);
        if (!go || !component) continue;
        for (const auto& entry : component->scripts) {
            if (!entry.script) { ++review.unavailableScripts; continue; }
            ++review.scriptsChecked;
            std::vector<scene::ScriptRequirementIssue> issues;
            scene::CollectScriptRequirementIssues(*go, *entry.script, issues, false, true);
            for (auto& issue : issues)
                review.rows.push_back({std::move(issue), entry.script->InspectionId()});
        }
    }
}

void RegisterScriptRequirementOperators(OperatorRegistry& registry)
{
    const auto available = [](const OpContext& c, const OpArgs&) {
        return CanReviewScriptRequirements(c.ctx);
    };
    EditorOperator validate;
    validate.id = "script.requirements.validate";
    validate.label = "必須設定をチェック";
    validate.category = "Inspector";
    validate.desc = "シーンの必須 Component・Script・Asset・参照を検証し、問題一覧を更新する。";
    validate.kind = OpKind::Action;
    validate.params = {{"showPanel", OpParamType::Bool, "結果パネルを開く", false, true}};
    validate.poll = available;
    validate.exec = [](OpContext& c, const OpArgs& args) {
        RefreshScriptRequirementReview(c.ctx);
        auto& review = c.ctx.scriptRequirementReview;
        if (args.GetBool("showPanel", true)) {
            OpArgs panel;
            panel.Set("panel", std::string("Script Requirements"));
            InvokeOperator(c.ctx, "panel.focus", panel);
            review.focusPanel = true;
        }
        OpData result = OpData::MakeObject();
        result.Set("revision", review.revision);
        result.Set("scriptsChecked", review.scriptsChecked);
        result.Set("unavailableScripts", review.unavailableScripts);
        auto rows = OpData::MakeArray();
        for (std::size_t i = 0; i < review.rows.size(); ++i) {
            const auto& row = review.rows[i];
            auto item = OpData::MakeObject();
            item.Set("index", static_cast<int>(i));
            item.Set("node", row.issue.instanceId);
            item.Set("object", row.issue.objectName);
            item.Set("script", row.issue.scriptType);
            item.Set("scriptId", row.scriptId);
            item.Set("field", row.issue.fieldKey);
            item.Set("reason", scene::FormatScriptRequirementIssue(row.issue));
            rows.Push(std::move(item));
        }
        result.Set("issues", std::move(rows));
        return OpResult::Data(std::move(result));
    };
    registry.Register(std::move(validate));

    EditorOperator reveal;
    reveal.id = "script.requirements.reveal";
    reveal.label = "必須設定の問題箇所へ移動";
    reveal.category = "Inspector";
    reveal.desc = "検証結果の対象を選択し、Inspector の該当 Script・項目を表示する。";
    reveal.kind = OpKind::Action;
    reveal.params = {{"revision", OpParamType::Int, "検証結果の revision"},
                     {"index", OpParamType::Int, "検証結果の行番号"}};
    reveal.poll = available;
    reveal.exec = [](OpContext& c, const OpArgs& args) -> OpResult {
        auto& review = c.ctx.scriptRequirementReview;
        const int index = args.GetInt("index", -1);
        if (!IsScriptRequirementReviewCurrent(c.ctx) ||
            args.GetInt("revision") != review.revision)
            return OpResult::Err("STALE_REVIEW", "検証結果が古いため、再検証してください。");
        if (index < 0 || static_cast<std::size_t>(index) >= review.rows.size())
            return OpResult::Err("BAD_INDEX", "検証結果の行が見つかりません。");
        const auto& row = review.rows[static_cast<std::size_t>(index)];
        auto* go = c.ctx.activeScene->FindByGuid(row.issue.instanceId);
        auto* component = go ? go->GetComponent<scene::ScriptComponent>() : nullptr;
        scene::Script* script = nullptr;
        if (component)
            for (const auto& entry : component->scripts)
                if (entry.script && entry.script->InspectionId() == row.scriptId)
                    script = entry.script.get();
        if (!script)
            return OpResult::Err("STALE_TARGET", "対象が削除・再読み込みされています。再検証してください。");
        SelectEntity(c.ctx, go->GetID(), SelectionReveal::Show);
        review.focus = {row.issue.instanceId, row.scriptId,
            row.issue.kind == scene::ScriptRequirementKind::Asset ||
            row.issue.kind == scene::ScriptRequirementKind::Reference ? row.issue.fieldKey : "",
            scene::FormatScriptRequirementIssue(row.issue), true, true};
        OpArgs panel;
        panel.Set("panel", std::string("Inspector"));
        InvokeOperator(c.ctx, "panel.focus", panel);
        return OpResult::Ok();
    };
    registry.Register(std::move(reveal));
}
} /// @note namespace fbzz::editor
