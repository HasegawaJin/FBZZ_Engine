// FBZZ Engine
// ScriptCursorProxy.hpp | fbzz::scene
// Script からマウスカーソル表示・拘束状態を制御するショートハンド
#pragma once

#include <Engine/Core/Cursor.hpp>

namespace fbzz::scene {

class Script;

using CursorLockMode = core::CursorLockMode;

struct ScriptCursorProxy {
    Script* script = nullptr;

    void SetVisible(bool visible) const;
    bool IsVisible() const;
    void SetLockMode(CursorLockMode mode) const;
    CursorLockMode GetLockMode() const;
    void ResetForEditor() const;
};

} // namespace fbzz::scene
