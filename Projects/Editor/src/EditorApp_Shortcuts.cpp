// FBZZ Engine
// EditorApp_Shortcuts.cpp | fbzz::editor
// ショートカット一覧オーバーレイ (F1) と選択ヒストリ (Alt+←/→)
//
// WHY: ホットキーは HotkeyManager が保持しているのに一覧で見る手段が無く、発見性が低かった。
//      また階層で選び直す手間を減らすため、直近の選択を往復できる履歴を用意する。
//      どちらも「操作の見通しを良くする」横断的 UX 改善であり、1 ファイルにまとめる。
#include <Editor/EditorApp.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/HotkeyManager.hpp>
#include <Engine/Scene/Scene.hpp>
#include <imgui.h>
#include <string>

namespace fbzz::editor {

namespace {

// Hotkey を "Ctrl+Shift+K" のような表示文字列へ整形する (HotkeyEditorPanel と同じ規則)。
std::string FormatBinding(const Hotkey& hk)
{
    std::string s;
    if (hk.ctrl)  s += "Ctrl+";
    if (hk.shift) s += "Shift+";
    if (hk.alt)   s += "Alt+";
    s += ImGui::GetKeyName(static_cast<ImGuiKey>(hk.imguiKey));
    return s;
}

} // namespace

// =============================================================================
// ショートカット一覧オーバーレイ (F1)
// =============================================================================

void EditorApp::DrawShortcutsOverlay(EditorContext& ctx)
{
    if (!m_showShortcutsOverlay) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { m_showShortcutsOverlay = false; return; }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, { 0.5f, 0.5f });
    ImGui::SetNextWindowSize({ 560.0f, 0.0f }, ImGuiCond_Appearing);
    ImGui::SetNextWindowBgAlpha(0.96f);

    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize;
    bool open = true;
    if (ImGui::Begin("Keyboard Shortcuts", &open, kFlags)) {
        ImGui::TextDisabled("Press F1 or Esc to close");
        ImGui::Separator();

        // ── 登録ホットキー (リバインド可能) ──────────────────────────────────
        if (ctx.hotkeyManager) {
            constexpr ImGuiTableFlags kTbl =
                ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg;
            if (ImGui::BeginTable("##hk_list", 2, kTbl)) {
                ImGui::TableSetupColumn("Action",   ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Shortcut", ImGuiTableColumnFlags_WidthFixed, 170.0f);
                ImGui::TableHeadersRow();
                for (const Hotkey& hk : ctx.hotkeyManager->GetHotkeys()) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(hk.name.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextColored({ 0.55f, 0.80f, 1.0f, 1.0f }, "%s",
                                       FormatBinding(hk).c_str());
                }
                ImGui::EndTable();
            }
        }

        // ── リバインド不可の主要操作 (視点・選択) を補足 ──────────────────────
        ImGui::Separator();
        ImGui::TextDisabled("Viewport / Selection");
        static constexpr struct { const char* action; const char* input; } kTips[] = {
            { "Look around",        "RMB drag" },
            { "Fly (while RMB)",    "W / A / S / D / Q / E" },
            { "Pan",                "MMB drag" },
            { "Zoom",               "Mouse wheel" },
            { "Focus selected",     "F" },
            { "Multi-select",       "Ctrl+Click" },
            { "Duplicate",          "Ctrl+D" },
            { "Delete",             "Del" },
            { "Selection back / forward", "Alt+Left / Alt+Right" },
        };
        constexpr ImGuiTableFlags kTbl2 =
            ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg;
        if (ImGui::BeginTable("##tips", 2, kTbl2)) {
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Input",  ImGuiTableColumnFlags_WidthFixed, 200.0f);
            for (const auto& t : kTips) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(t.action);
                ImGui::TableSetColumnIndex(1); ImGui::TextDisabled("%s", t.input);
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
    if (!open) m_showShortcutsOverlay = false;
}

// =============================================================================
// 選択ヒストリ (Alt+←/→)
// =============================================================================

void EditorApp::RecordSelectionHistory()
{
    const scene::EntityID cur = m_ctx.PrimarySelected();

    // 履歴移動由来の選択は新規記録しない (往復で履歴が汚れるのを防ぐ)。
    if (m_selectionNavigating) {
        m_selectionNavigating   = false;
        m_lastRecordedSelection = cur;
        return;
    }
    if (cur == m_lastRecordedSelection) return;
    m_lastRecordedSelection = cur;
    if (!cur.IsValid()) return; // 選択解除は履歴に積まない

    // 現在位置より先 (redo 側) を捨ててから積む。
    if (m_selectionHistoryIndex + 1 < static_cast<int>(m_selectionHistory.size()))
        m_selectionHistory.resize(m_selectionHistoryIndex + 1);
    m_selectionHistory.push_back(cur);

    // 上限を超えたら古い履歴から捨てる。
    constexpr int kMaxHistory = 64;
    if (static_cast<int>(m_selectionHistory.size()) > kMaxHistory)
        m_selectionHistory.erase(m_selectionHistory.begin());
    m_selectionHistoryIndex = static_cast<int>(m_selectionHistory.size()) - 1;
}

void EditorApp::NavigateSelectionHistory(int dir)
{
    if (!m_ctx.activeScene || m_selectionHistory.empty()) return;

    int idx = m_selectionHistoryIndex + dir;
    // 削除済みエンティティはスキップし、生きている履歴項目まで進む。
    while (idx >= 0 && idx < static_cast<int>(m_selectionHistory.size())) {
        const scene::EntityID id = m_selectionHistory[idx];
        if (m_ctx.activeScene->GetGameObject(id)) {
            m_selectionHistoryIndex = idx;
            m_ctx.selectedEntities  = { id };
            m_selectionNavigating   = true; // この選択は履歴へ再記録しない
            m_lastRecordedSelection = id;
            return;
        }
        idx += dir;
    }
}

} // namespace fbzz::editor
