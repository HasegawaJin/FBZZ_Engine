// FBZZ Engine
// HotkeyEditorPanel.cpp | fbzz::editor
// ホットキー一覧表示とリバインド UI
#include <Editor/Panels/HotkeyEditorPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/HotkeyManager.hpp>
#include <imgui.h>

namespace fbzz::editor {

namespace {

const char* ImGuiKeyName(int key)
{
    return ImGui::GetKeyName(static_cast<ImGuiKey>(key));
}

std::string FormatBinding(const Hotkey& hk)
{
    std::string s;
    if (hk.ctrl)  s += "Ctrl+";
    if (hk.shift) s += "Shift+";
    if (hk.alt)   s += "Alt+";
    s += ImGuiKeyName(hk.imguiKey);
    return s;
}

} // namespace

void HotkeyEditorPanel::OnRenderContent(EditorContext& ctx)
{
    if (!ctx.hotkeyManager) {
        ImGui::TextDisabled("HotkeyManager not available.");
        return;
    }

    // リバインド待ち中: 次のキー入力を捕捉する
    if (!m_rebindTarget.empty()) {
        ImGui::TextColored({ 0.4f, 0.9f, 1.0f, 1.0f },
            "Press any key to rebind \"%s\"  (Esc = cancel)",
            m_rebindTarget.c_str());
        ImGui::Separator();

        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_rebindTarget.clear();
        } else {
            for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
                if (k == ImGuiKey_Escape) continue;
                if (k == ImGuiKey_LeftCtrl  || k == ImGuiKey_RightCtrl)  continue;
                if (k == ImGuiKey_LeftShift || k == ImGuiKey_RightShift) continue;
                if (k == ImGuiKey_LeftAlt   || k == ImGuiKey_RightAlt)   continue;
                if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(k))) {
                    const bool ctrl  = ImGui::IsKeyDown(ImGuiKey_LeftCtrl)  || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
                    const bool shift = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);
                    const bool alt   = ImGui::IsKeyDown(ImGuiKey_LeftAlt)   || ImGui::IsKeyDown(ImGuiKey_RightAlt);
                    ctx.hotkeyManager->Rebind(m_rebindTarget, k, ctrl, shift, alt);
                    m_rebindTarget.clear();
                    break;
                }
            }
        }
    }

    const auto& hotkeys = ctx.hotkeyManager->GetHotkeys();
    if (hotkeys.empty()) {
        ImGui::TextDisabled("No hotkeys registered.");
        return;
    }

    if (ImGui::BeginTable("##hotkeys", 3,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Action",  ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Binding", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn("",        ImGuiTableColumnFlags_WidthFixed,   80.0f);
        ImGui::TableHeadersRow();

        for (const auto& hk : hotkeys) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(hk.name.c_str());

            ImGui::TableSetColumnIndex(1);
            const bool waiting = (m_rebindTarget == hk.name);
            if (waiting) {
                ImGui::TextColored({ 0.4f, 0.9f, 1.0f, 1.0f }, "...");
            } else {
                ImGui::TextUnformatted(FormatBinding(hk).c_str());
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::PushID(hk.name.c_str());
            if (ImGui::SmallButton("Rebind")) {
                m_rebindTarget = hk.name;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (ImGui::Button("Reset All to Defaults")) {
        ctx.hotkeyManager->Clear();
        // EditorApp::RegisterDefaultHotkeys に相当するリセットはここからは呼べないため、
        // 再起動を促すヒントを表示する。
        ImGui::OpenPopup("##hk_reset_note");
    }
    if (ImGui::BeginPopup("##hk_reset_note")) {
        ImGui::TextUnformatted("Restart the editor to apply defaults.");
        if (ImGui::Button("OK")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace fbzz::editor
