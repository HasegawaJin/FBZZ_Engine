/// @file    ScriptCursorProxy.hpp
/// @brief   Script からマウスカーソル表示・拘束状態を制御するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-26
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
