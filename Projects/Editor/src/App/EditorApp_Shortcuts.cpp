/// @file    EditorApp_Shortcuts.cpp
/// @brief   ショートカット一覧オーバーレイ (F1) と選択ヒストリ (Alt+←/→)。
/// @author  Hasegawa Jin
/// @date    2026-07-20
///
/// @note ホットキー一覧オーバーレイと選択ヒストリの横断的 UX 改善を 1 ファイルにまとめる。
#include <Editor/EditorApp.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Op/HotkeyManager.hpp>
#include <Editor/UI/Localization.hpp>
#include <Editor/Scene/Editing/Selection.hpp>
#include <Engine/Scene/Scene.hpp>
#include <imgui.h>
#include <algorithm>
#include <string>

namespace fbzz::editor {

/// @name ショートカット一覧オーバーレイ (F1)
/// @note 表の中身は HotkeyManager の登録内容から丸ごと生成する (手書きの表は更新漏れで
///       Ctrl+A / Esc / W・E・R・Q が一覧から抜け落ちていた)。

void EditorApp::DrawShortcutsOverlay(EditorContext& ctx)
{
    if (!m_showShortcutsOverlay) {
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        m_showShortcutsOverlay = false;
        return;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, { 0.5f, 0.5f });
    ImGui::SetNextWindowSize({ std::min(620.0f, vp->WorkSize.x * 0.9f),
                               std::min(640.0f, vp->WorkSize.y * 0.9f) }, ImGuiCond_Appearing);
    ImGui::SetNextWindowBgAlpha(0.96f);

    bool open = true;
    if (ImGui::Begin("Keyboard Shortcuts", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::TextDisabled("%s", LOCT("Press Esc to close"));
        ImGui::TextWrapped("%s", LOCT("Greyed-out shortcuts are unavailable for the current selection or panel."));
        ImGui::Separator();

        if (!ctx.hotkeyManager) {
            ImGui::TextDisabled("HotkeyManager is not available.");
            ImGui::End();
            return;
        }

        // カテゴリごとに区切って出す。並び順は HotkeyCategory の宣言順。
        static constexpr HotkeyCategory kOrder[] = {
            HotkeyCategory::File,      HotkeyCategory::Edit,
            HotkeyCategory::Selection, HotkeyCategory::Viewport,
            HotkeyCategory::Gizmo,     HotkeyCategory::Play,
            HotkeyCategory::Panels,    HotkeyCategory::Tools,
        };

        const auto& hotkeys = ctx.hotkeyManager->GetHotkeys();
        for (const HotkeyCategory category : kOrder) {
            bool headerDrawn = false;
            for (const Hotkey& hk : hotkeys) {
                if (hk.category != category) {
                    continue;
                }

                if (!headerDrawn) {
                    ImGui::SeparatorText(HotkeyManager::CategoryLabel(category));
                    constexpr ImGuiTableFlags kFlags =
                        ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg;
                    if (!ImGui::BeginTable(HotkeyManager::CategoryLabel(category), 2, kFlags)) {
                        break;
                    }
                    ImGui::TableSetupColumn("Action",   ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Shortcut", ImGuiTableColumnFlags_WidthStretch);
                    headerDrawn = true;
                }

                // 今この瞬間に発火しうるかで濃淡を変える。「一覧に出ているのに押しても
                // 何も起きない」が一番混乱するため、条件 (選択がある/Play 中でない等) を見た目に出す。
                const bool active = ctx.hotkeyManager->IsCurrentlyActive(hk);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                // 一覧の名前は operator の label と同じ出所なので、メニューと同じ訳を出す
                // (片方だけ日本語だと «同じ操作» に見えなくなる)。
                const char* shownName = LOCT(hk.name.c_str());
                if (active) {
                    ImGui::TextUnformatted(shownName);
                } else {
                    ImGui::TextDisabled("%s", shownName);
                }

                ImGui::TableSetColumnIndex(1);
                const std::string binding = HotkeyManager::FormatBinding(hk);
                if (active) {
                    ImGui::TextColored({ 0.55f, 0.80f, 1.0f, 1.0f }, "%s", binding.c_str());
                } else {
                    ImGui::TextDisabled("%s", binding.c_str());
                }
            }
            if (headerDrawn) {
                ImGui::EndTable();
            }
        }
    }
    ImGui::End();

    if (!open) {
        m_showShortcutsOverlay = false;
    }
}

/// 選択ヒストリ (Alt+←/→)

void EditorApp::RecordSelectionHistory()
{
    const scene::EntityID cur = m_ctx.PrimarySelected();

    // 履歴移動由来の選択は新規記録しない (往復で履歴が汚れるのを防ぐ)。
    if (m_selectionNavigating) {
        m_selectionNavigating   = false;
        m_lastRecordedSelection = cur;
        return;
    }
    if (cur == m_lastRecordedSelection) {
        return;
    }
    m_lastRecordedSelection = cur;
    // 選択解除は履歴に積まない
    if (!cur.IsValid()) {
        return;
    }

    // 現在位置より先 (redo 側) を捨ててから積む。
    if (m_selectionHistoryIndex + 1 < static_cast<int>(m_selectionHistory.size())) {
        m_selectionHistory.resize(m_selectionHistoryIndex + 1);
    }
    m_selectionHistory.push_back(cur);

    // 上限を超えたら古い履歴から捨てる。
    constexpr int kMaxHistory = 64;
    if (static_cast<int>(m_selectionHistory.size()) > kMaxHistory) {
        m_selectionHistory.erase(m_selectionHistory.begin());
    }
    m_selectionHistoryIndex = static_cast<int>(m_selectionHistory.size()) - 1;
}

void EditorApp::NavigateSelectionHistory(int dir)
{
    if (!m_ctx.activeScene || m_selectionHistory.empty()) {
        return;
    }

    int idx = m_selectionHistoryIndex + dir;
    // 削除済みエンティティはスキップし、生きている履歴項目まで進む。
    while (idx >= 0 && idx < static_cast<int>(m_selectionHistory.size())) {
        const scene::EntityID id = m_selectionHistory[idx];
        if (m_ctx.activeScene->GetGameObject(id)) {
            m_selectionHistoryIndex = idx;
            SelectEntity(m_ctx, id);
            // この選択は履歴へ再記録しない
            m_selectionNavigating   = true;
            m_lastRecordedSelection = id;
            return;
        }
        idx += dir;
    }
}

} // namespace fbzz::editor
