// FBZZ Engine
// Input.cpp | fbzz::input
// 入力状態の更新と照会
// Win32 メッセージで現在状態を更新し、Update で前フレーム状態を保存する。
// KeyDown / MouseButtonDown は現在と前回の差分から判定する。
#include "Engine/Input/Input.hpp"

#include <windowsx.h>

namespace fbzz::input {

std::array<bool, Input::KEY_COUNT> Input::s_current      = {};
std::array<bool, Input::KEY_COUNT> Input::s_previous     = {};
std::array<bool, 3>                Input::s_mouseCurrent  = {};
std::array<bool, 3>                Input::s_mousePrevious = {};
math::Vector2                      Input::s_mousePos      = {};
math::Vector2                      Input::s_prevMousePos  = {};
float                              Input::s_scrollDelta   = 0.0f;

void Input::Init()
{
    Reset();
}

void Input::Reset()
{
    s_current.fill(false);
    s_previous.fill(false);
    s_mouseCurrent.fill(false);
    s_mousePrevious.fill(false);
    s_mousePos     = {};
    s_prevMousePos = {};
    s_scrollDelta  = 0.0f;
}

void Input::Update()
{
    s_previous      = s_current;
    s_mousePrevious = s_mouseCurrent;
    s_prevMousePos  = s_mousePos;
    s_scrollDelta   = 0.0f;
}

bool Input::KeyDown(KeyCode key)
{
    const int k = static_cast<int>(key);
    return s_current[k] && !s_previous[k];
}

bool Input::KeyHeld(KeyCode key)
{
    return s_current[static_cast<int>(key)];
}

bool Input::KeyUp(KeyCode key)
{
    const int k = static_cast<int>(key);
    return !s_current[k] && s_previous[k];
}

bool Input::MouseButton    (int b) { return  s_mouseCurrent[b]; }
bool Input::MouseButtonDown(int b) { return  s_mouseCurrent[b] && !s_mousePrevious[b]; }
bool Input::MouseButtonUp  (int b) { return !s_mouseCurrent[b] &&  s_mousePrevious[b]; }

math::Vector2 Input::MousePosition() { return s_mousePos; }
math::Vector2 Input::MouseDelta()
{
    return { s_mousePos.x - s_prevMousePos.x, s_mousePos.y - s_prevMousePos.y };
}

void Input::HandleKeyMessage(UINT msg, WPARAM wParam)
{
    if (wParam >= static_cast<WPARAM>(KEY_COUNT)) return;
    s_current[wParam] = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
}

void Input::HandleMouseMove(int x, int y)
{
    s_mousePos = { static_cast<float>(x), static_cast<float>(y) };
}

float Input::MouseScrollDelta()
{
    return s_scrollDelta;
}

void Input::HandleMouseScroll(float delta) { s_scrollDelta += delta; }

void Input::HandleMouseButton(UINT msg)
{
    switch (msg)
    {
    case WM_LBUTTONDOWN: s_mouseCurrent[0] = true;  s_current[VK_LBUTTON] = true;  break;
    case WM_LBUTTONUP:   s_mouseCurrent[0] = false; s_current[VK_LBUTTON] = false; break;
    case WM_RBUTTONDOWN: s_mouseCurrent[1] = true;  s_current[VK_RBUTTON] = true;  break;
    case WM_RBUTTONUP:   s_mouseCurrent[1] = false; s_current[VK_RBUTTON] = false; break;
    case WM_MBUTTONDOWN: s_mouseCurrent[2] = true;  s_current[VK_MBUTTON] = true;  break;
    case WM_MBUTTONUP:   s_mouseCurrent[2] = false; s_current[VK_MBUTTON] = false; break;
    }
}

} // namespace fbzz::input
