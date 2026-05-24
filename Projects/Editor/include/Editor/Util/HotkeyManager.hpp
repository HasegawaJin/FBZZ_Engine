// FBZZ Engine
// HotkeyManager.hpp | fbzz::editor
// グローバルキーショートカットの登録と処理
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace fbzz::editor {

struct Hotkey {
    std::string           name;
    int                   imguiKey;
    bool                  ctrl  = false;
    bool                  shift = false;
    bool                  alt   = false;
    std::function<void()> callback;
};

class HotkeyManager {
public:
    void Register(Hotkey hotkey);
    void ProcessInput();  // 毎フレーム EditorApp から呼ぶ
    void Clear();

private:
    std::vector<Hotkey> m_hotkeys;
};

} // namespace fbzz::editor
