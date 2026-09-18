/// @file    UIPointer.cpp
/// @brief   UI ポインター差し替えの実体
/// @author  Hasegawa Jin
/// @date    2026-08-24
#include <Engine/Scene/UIPointer.hpp>

#include <Engine/Core/Time.hpp>

namespace fbzz::scene {

math::Vector2 UIPointer::s_position{};
bool          UIPointer::s_pressed = false;
uint64_t      UIPointer::s_frame   = 0;

void UIPointer::Set(const math::Vector2& canvasPosition, bool pressed)
{
    s_position = canvasPosition;
    s_pressed  = pressed;
    /// @note 0 は「未設定」の意味に使うため、起動直後の frameCount == 0 でも生きた値になるよう +1 する。
    s_frame    = Time::frameCount + 1;
}

void UIPointer::Clear()
{
    s_frame = 0;
}

bool UIPointer::IsActive()
{
    if (s_frame == 0) return false;
    const uint64_t now = Time::frameCount + 1;
    /// @note UI の更新はスクリプトの後に走るとは限らない。同じフレーム内の順序に依存させると、
    ///       順序が変わった瞬間にカーソルが 1 フレームおきに点滅するため 1 フレームの猶予を持たせる。
    return now <= s_frame + 1;
}

math::Vector2 UIPointer::Position() { return s_position; }
bool          UIPointer::Pressed()  { return s_pressed; }

} // namespace fbzz::scene
