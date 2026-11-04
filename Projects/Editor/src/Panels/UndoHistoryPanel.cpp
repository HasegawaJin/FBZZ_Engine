/// @file    UndoHistoryPanel.cpp
/// @brief   Undo 履歴パネル実装。
/// @author  Hasegawa Jin
/// @date    2026-06-16
#include <Editor/Panels/UndoHistoryPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <imgui.h>

namespace fbzz::editor {

void UndoHistoryPanel::OnRenderContent(EditorContext& ctx)
{
    if (!ctx.undoStack) {
        ImGui::TextDisabled("No undo stack");
        return;
    }

    UndoStack& stack = *ctx.undoStack;

    /// @note Clear ボタン
    const bool canClear = stack.GetHistorySize() > 0;
    if (!canClear) ImGui::BeginDisabled();
    if (ImGui::Button("Clear History"))
        { OpArgs args; args.Set("action", std::string("clear")); InvokeOperator(ctx, "edit.history.control", args); }
    if (!canClear) ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextDisabled("%zu / %zu", stack.GetCursor(), stack.GetHistorySize());

    ImGui::Separator();

    const auto entries   = stack.GetHistory();
    const std::size_t sz = entries.size();

    /// @note 一番上に「初期状態」エントリ
    {
        const bool isCurrent = (stack.GetCursor() == 0);
        ImGui::PushStyleColor(ImGuiCol_Text,
            isCurrent ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
        if (ImGui::Selectable("<Initial State>", isCurrent))
            { OpArgs args; args.Set("action", std::string("jump")); args.Set("index", 0); InvokeOperator(ctx, "edit.history.control", args); }
        ImGui::PopStyleColor();
    }

    for (std::size_t i = 0; i < sz; ++i) {
        const auto& e       = entries[i];
        const bool isCurrent = (stack.GetCursor() == i + 1);
        const bool isApplied = e.applied;

        ImVec4 color;
        /// @note 緑: 現在位置
        if (isCurrent)      color = { 0.4f, 0.9f, 0.4f, 1.0f };
        /// @note 白: 適用済み
        else if (isApplied) color = { 0.85f, 0.85f, 0.85f, 1.0f };
        /// @note グレー: Redo 側
        else                color = { 0.45f, 0.45f, 0.45f, 1.0f };

        ImGui::PushStyleColor(ImGuiCol_Text, color);
        char label[256];
        std::snprintf(label, sizeof(label), "%s##hist%zu", e.description.c_str(), i);
        if (ImGui::Selectable(label, isCurrent))
            { OpArgs args; args.Set("action", std::string("jump")); args.Set("index", static_cast<int>(i + 1)); InvokeOperator(ctx, "edit.history.control", args); }
        ImGui::PopStyleColor();

        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Jump to this state");
    }

    /// @note 現在位置が画面外のとき自動スクロール
    if (stack.GetRevision() != m_lastRevision) {
        m_lastRevision = stack.GetRevision();
        ImGui::SetScrollHereY(0.5f);
    }
}

} /// @note namespace fbzz::editor
