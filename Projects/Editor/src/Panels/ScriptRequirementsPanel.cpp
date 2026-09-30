/// @file    ScriptRequirementsPanel.cpp
/// @brief   必須設定の問題一覧から選択・再検証する UI。
/// @author  Hasegawa Jin
/// @date    2026-09-29
#include <Editor/Panels/ScriptRequirementsPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>

namespace fbzz::editor {
void ScriptRequirementsPanel::OnBeforeBegin(EditorContext& ctx)
{
    ImGui::SetNextWindowSize({720, 420}, ImGuiCond_FirstUseEver);
    if (ctx.scriptRequirementReview.focusPanel) {
        ImGui::SetNextWindowFocus();
        ImGui::SetNextWindowCollapsed(false);
        ctx.scriptRequirementReview.focusPanel = false;
    }
}

void ScriptRequirementsPanel::OnRenderContent(EditorContext& ctx)
{
    auto& review = ctx.scriptRequirementReview;
    const bool available = CanInvokeOperator(ctx, "script.requirements.validate");
    ImGui::BeginDisabled(!available);
    if (ImGui::Button(review.checked ? "再検証" : "必須設定をチェック")) {
        OpArgs args;
        args.Set("showPanel", false);
        InvokeOperator(ctx, "script.requirements.validate", args);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("変更後は再検証してください");
    if (!available)
        ImGui::TextWrapped("編集モードでシーンを開き、Script の読み込み完了後に検証できます。");
    if (!review.message.empty()) ImGui::TextWrapped("%s", review.message.c_str());
    if (!IsScriptRequirementReviewCurrent(ctx)) {
        ImGui::TextWrapped("このシーンの検証結果はありません。チェックを実行してください。");
        return;
    }
    ImGui::Separator();
    ImGui::Text("検証時点: %d Script / 問題 %zu 件", review.scriptsChecked, review.rows.size());
    if (review.unavailableScripts)
        ImGui::TextWrapped("未ロードの Script が %d 件あるため検証は未完了です。ビルド・読み込み後に再検証してください。", review.unavailableScripts);
    else if (review.rows.empty())
        ImGui::TextColored({0.35f, 0.85f, 0.5f, 1}, "検証時点で必須設定の不足はありません。");
    ImGui::TextWrapped("項目をクリックすると Inspector に移動します。Awake で設定する参照は再生時に再判定されます。");
    if (ImGui::BeginTable("requirements", 4, ImGuiTableFlags_BordersInnerH |
        ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("対象");
        ImGui::TableSetupColumn("Script");
        ImGui::TableSetupColumn("項目");
        ImGui::TableSetupColumn("理由");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < review.rows.size(); ++i) {
            const auto& row = review.rows[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!available);
            if (ImGui::Selectable((row.issue.objectName + "###issue").c_str(),
                false, ImGuiSelectableFlags_SpanAllColumns)) {
                OpArgs args;
                args.Set("index", static_cast<int>(i));
                args.Set("revision", review.revision);
                const auto result = InvokeOperator(ctx, "script.requirements.reveal", args);
                review.message = result.ok ? "" : result.message;
            }
            ImGui::EndDisabled();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.issue.scriptType.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.issue.fieldKey.empty() ? row.issue.componentDisplay.c_str() : row.issue.fieldKey.c_str());
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", scene::FormatScriptRequirementIssue(row.issue).c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}
} /// @note namespace fbzz::editor
