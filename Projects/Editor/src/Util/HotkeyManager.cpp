// FBZZ Engine
// HotkeyManager.cpp | fbzz::editor
// キーショートカットの登録・判定・整形
#include <Editor/Util/HotkeyManager.hpp>
#include <imgui.h>

#include <utility>

namespace fbzz::editor {

void HotkeyManager::Register(Hotkey hotkey)
{
    m_hotkeys.push_back(std::move(hotkey));
}

void HotkeyManager::RegisterInfo(std::string name,
                                 std::string binding,
                                 HotkeyCategory category,
                                 HotkeyScope scope)
{
    Hotkey hk;
    hk.name        = std::move(name);
    hk.infoBinding = std::move(binding);
    hk.category    = category;
    hk.scope       = scope;
    hk.infoOnly    = true;
    m_hotkeys.push_back(std::move(hk));
}

bool HotkeyManager::ScopeActive(HotkeyScope scope) const
{
    if (HasScope(scope, HotkeyScope::Global)) return true;
    if (!m_scopeResolver) return false;   // 解決手段が無いなら Global 以外は発火させない
    return m_scopeResolver(scope);
}

bool HotkeyManager::IsCurrentlyActive(const Hotkey& hk) const
{
    if (hk.infoOnly) return false;
    if (!ScopeActive(hk.scope)) return false;
    if (hk.enabled && !hk.enabled()) return false;
    return true;
}

void HotkeyManager::ProcessInput()
{
    // WHY: テキスト入力中はキーが文字として消費される。名前入力の途中で
    //      "D" がオブジェクト複製になってはいけない。
    if (ImGui::GetIO().WantTextInput) return;

    const ImGuiIO& io = ImGui::GetIO();
    for (const auto& hk : m_hotkeys) {
        if (hk.infoOnly || !hk.callback) continue;

        // 修飾キーは完全一致を要求する。
        // WHY: 部分一致にすると Ctrl+Z (Undo) が Z (Pivot 切替) も同時に発火させる。
        if (hk.ctrl != io.KeyCtrl || hk.shift != io.KeyShift || hk.alt != io.KeyAlt)
            continue;
        if (!ImGui::IsKeyPressed(static_cast<ImGuiKey>(hk.imguiKey), false)) continue;
        if (!IsCurrentlyActive(hk)) continue;

        hk.callback();
    }
}

void HotkeyManager::Clear() { m_hotkeys.clear(); }

void HotkeyManager::Rebind(const std::string& name, int imguiKey, bool ctrl, bool shift, bool alt)
{
    for (auto& hk : m_hotkeys) {
        if (hk.name != name) continue;
        if (hk.infoOnly) return;   // 説明専用エントリは割り当てを持たない
        hk.imguiKey = imguiKey;
        hk.ctrl     = ctrl;
        hk.shift    = shift;
        hk.alt      = alt;
        return;
    }
}

std::string HotkeyManager::FindConflict(const std::string& name,
                                        int imguiKey, bool ctrl, bool shift, bool alt) const
{
    // 対象自身の scope を引く (見つからなければ Global 扱いで最も厳しく判定する)。
    HotkeyScope selfScope = HotkeyScope::Global;
    for (const auto& hk : m_hotkeys)
        if (hk.name == name) { selfScope = hk.scope; break; }

    for (const auto& hk : m_hotkeys) {
        if (hk.infoOnly || hk.name == name) continue;
        if (hk.imguiKey != imguiKey || hk.ctrl != ctrl || hk.shift != shift || hk.alt != alt)
            continue;

        // 文脈が重ならないなら共存できる。
        // WHY: Scene View の Delete と Hierarchy の Delete は同じキーでよく、
        //      むしろ揃っている方が自然。Global はどこでも効くので必ず衝突する。
        const bool conflicts =
            HasScope(hk.scope, HotkeyScope::Global) ||
            HasScope(selfScope, HotkeyScope::Global) ||
            (static_cast<std::uint32_t>(hk.scope) & static_cast<std::uint32_t>(selfScope)) != 0;
        if (conflicts) return hk.name;
    }
    return {};
}

std::string HotkeyManager::FormatBinding(const Hotkey& hk)
{
    if (hk.infoOnly) return hk.infoBinding;

    std::string s;
    if (hk.ctrl)  s += "Ctrl+";
    if (hk.shift) s += "Shift+";
    if (hk.alt)   s += "Alt+";
    if (const char* keyName = ImGui::GetKeyName(static_cast<ImGuiKey>(hk.imguiKey)))
        s += keyName;
    return s;
}

const char* HotkeyManager::CategoryLabel(HotkeyCategory category)
{
    switch (category) {
    case HotkeyCategory::File:      return "File";
    case HotkeyCategory::Edit:      return "Edit";
    case HotkeyCategory::Selection: return "Selection";
    case HotkeyCategory::Viewport:  return "Viewport";
    case HotkeyCategory::Gizmo:     return "Gizmo";
    case HotkeyCategory::Play:      return "Play";
    case HotkeyCategory::Panels:    return "Panels";
    }
    return "Other";
}

} // namespace fbzz::editor
