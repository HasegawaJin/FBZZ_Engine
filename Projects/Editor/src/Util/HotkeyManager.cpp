// FBZZ Engine
// HotkeyManager.cpp | fbzz::editor
// グローバルキーショートカットの処理
#include <Editor/Util/HotkeyManager.hpp>
#include <imgui.h>

namespace fbzz::editor {

void HotkeyManager::Register(Hotkey hotkey)
{
    m_hotkeys.push_back(std::move(hotkey));
}

void HotkeyManager::ProcessInput()
{
    if (ImGui::GetIO().WantTextInput) return;
    for (const auto& hk : m_hotkeys) {
        bool modOk = hk.ctrl  == ImGui::GetIO().KeyCtrl
                  && hk.shift == ImGui::GetIO().KeyShift
                  && hk.alt   == ImGui::GetIO().KeyAlt;
        if (modOk && ImGui::IsKeyPressed(static_cast<ImGuiKey>(hk.imguiKey), false))
            hk.callback();
    }
}

void HotkeyManager::Clear() { m_hotkeys.clear(); }

void HotkeyManager::Rebind(const std::string& name, int imguiKey, bool ctrl, bool shift, bool alt)
{
    for (auto& hk : m_hotkeys) {
        if (hk.name == name) {
            hk.imguiKey = imguiKey;
            hk.ctrl     = ctrl;
            hk.shift    = shift;
            hk.alt      = alt;
            return;
        }
    }
}

} // namespace fbzz::editor
