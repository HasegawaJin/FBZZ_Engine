/// @file    UndoHistoryPanel.cpp
/// @brief   Undo 履歴パネル実装。
/// @author  Hasegawa Jin
/// @date    2026-06-16
#include <Editor/Panels/Scene/UndoHistoryPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Scene/Editing/UndoStack.hpp>
#include <Editor/UI/Widgets/ModalDialog.hpp>
#include <Editor/UI/Localization.hpp>
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

    // Clear ボタン
    const bool canClear = stack.GetHistorySize() > 0;
    if (!canClear) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(LOCT("Clear History"))) {
        const auto revision = stack.GetRevision();
        ModalDialog::OpenConfirm(LOCT("Clear History"), LOCT("Clear this undo and redo history?"),
            [&ctx, expectedStack = &stack, revision] {
                if (ctx.undoStack != expectedStack || expectedStack->GetRevision() != revision) {
                    return;
                }
                OpArgs args;
                args.Set("action", std::string("clear"));
                InvokeOperator(ctx, "edit.history.control", args);
            });
    }
    if (!canClear) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    ImGui::TextDisabled("%zu / %zu", stack.GetCursor(), stack.GetHistorySize());

    ImGui::Separator();

    const auto entries   = stack.GetHistory();
    const std::size_t sz = entries.size();

    // 一番上に「初期状態」エントリ
    {
        const bool isCurrent = (stack.GetCursor() == 0);
        ImGui::PushStyleColor(ImGuiCol_Text,
            isCurrent ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
        if (ImGui::Selectable("<Initial State>", isCurrent)) {
            OpArgs args;
            args.Set("action", std::string("jump"));
            args.Set("index", 0);
            InvokeOperator(ctx, "edit.history.control", args);
        }
        if (isCurrent && stack.GetRevision() != m_lastRevision) {
            ImGui::SetScrollHereY(0.5f);
        }
        ImGui::PopStyleColor();
    }

    for (std::size_t i = 0; i < sz; ++i) {
        const auto& e       = entries[i];
        const bool isCurrent = (stack.GetCursor() == i + 1);
        const bool isApplied = e.applied;

        ImVec4 color;
        // 緑: 現在位置
        if (isCurrent) {
            color = { 0.4f, 0.9f, 0.4f, 1.0f };
        }
        // 白: 適用済み
        else if (isApplied) {
            color = { 0.85f, 0.85f, 0.85f, 1.0f };
        }
        // グレー: Redo 側
        else {
            color = { 0.45f, 0.45f, 0.45f, 1.0f };
        }

        ImGui::PushStyleColor(ImGuiCol_Text, color);
        char label[256];
        std::snprintf(label, sizeof(label), "%s##hist%zu", e.description.c_str(), i);
        if (ImGui::Selectable(label, isCurrent)) {
            OpArgs args;
            args.Set("action", std::string("jump"));
            args.Set("index", static_cast<int>(i + 1));
            InvokeOperator(ctx, "edit.history.control", args);
        }
        ImGui::PopStyleColor();

        if (isCurrent && stack.GetRevision() != m_lastRevision) {
            ImGui::SetScrollHereY(0.5f);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Jump to this state");
        }
    }

    // 現在位置が画面外のとき自動スクロール
    m_lastRevision = stack.GetRevision();
}

} // namespace fbzz::editor
