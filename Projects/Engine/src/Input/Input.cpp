/// @file    Input.cpp
/// @brief   入力状態の更新と照会。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// Win32 メッセージで現在状態を更新し、Update で前フレーム状態を保存する。
/// KeyDown / MouseButtonDown は現在と前回の差分から判定する。
#include "Engine/Input/Input.hpp"
#include "Engine/Input/Gamepad.hpp"

#include <algorithm>
#include <cmath>
#include <windowsx.h>

namespace fbzz::input {

std::array<bool, Input::KEY_COUNT> Input::s_current      = {};
std::array<bool, Input::KEY_COUNT> Input::s_previous     = {};
std::array<bool, 3>                Input::s_mouseCurrent  = {};
std::array<bool, 3>                Input::s_mousePrevious = {};
math::Vector2                      Input::s_mousePos      = {};
math::Vector2                      Input::s_prevMousePos  = {};
math::Vector2                      Input::s_overrideMouseDelta = {};
float                              Input::s_scrollDelta   = 0.0f;
bool                               Input::s_hasOverrideMouseDelta = false;
std::array<bool, Input::KEY_COUNT> Input::s_injectedKeys = {};
std::array<bool, 3>                Input::s_injectedMouseButtons = {};
std::unordered_map<std::string, float> Input::s_virtualAxes;
std::unordered_map<std::string, bool> Input::s_virtualButtons;
std::unordered_map<std::string, bool> Input::s_previousVirtualButtons;
std::string Input::s_textInput;

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
    s_overrideMouseDelta = {};
    s_scrollDelta  = 0.0f;
    s_hasOverrideMouseDelta = false;
    s_injectedKeys.fill(false);
    s_injectedMouseButtons.fill(false);
    s_virtualAxes.clear();
    s_virtualButtons.clear();
    s_previousVirtualButtons.clear();
    s_textInput.clear();

    // ゲームパッドの押下状態も一緒に落とす。
    // WHY: Play モードの開始・終了で Input だけリセットすると、パッドのボタンが
    //      押されたままの状態で持ち越され、Play 開始直後に意図しない入力が発火する。
    Gamepad::Reset();
}

void Input::Update()
{
    // ゲームパッドは Win32 メッセージポンプに乗らないため、ここで明示的にポーリングする。
    // WHY Input::Update() に内包するか: 入力更新の呼び出し点は Editor / Standalone /
    //     Sandbox に散らばっており、それぞれへ追記させると呼び忘れで
    //     「特定の実行経路でだけパッドが効かない」再現困難な不具合になる。
    Gamepad::Update();

    // 物理入力とAI注入を合成した値を前フレームへ保存し、注入でもDown/Upを正しく生成する。
    for (size_t index = 0; index < s_current.size(); ++index) {
        s_previous[index] = s_current[index] || s_injectedKeys[index];
    }
    for (size_t index = 0; index < s_mouseCurrent.size(); ++index) {
        s_mousePrevious[index] = s_mouseCurrent[index] || s_injectedMouseButtons[index];
    }
    s_previousVirtualButtons = s_virtualButtons;
    s_prevMousePos  = s_mousePos;
    s_overrideMouseDelta = {};
    s_scrollDelta   = 0.0f;
    s_hasOverrideMouseDelta = false;
    s_textInput.clear();
}

bool Input::KeyDown(KeyCode key)
{
    const int k = static_cast<int>(key);
    return (s_current[k] || s_injectedKeys[k]) && !s_previous[k];
}

bool Input::KeyHeld(KeyCode key)
{
    const int k = static_cast<int>(key);
    return s_current[k] || s_injectedKeys[k];
}

bool Input::KeyUp(KeyCode key)
{
    const int k = static_cast<int>(key);
    return !(s_current[k] || s_injectedKeys[k]) && s_previous[k];
}

bool Input::MouseButton    (int b) { return  s_mouseCurrent[b] || s_injectedMouseButtons[b]; }
bool Input::MouseButtonDown(int b) { return (s_mouseCurrent[b] || s_injectedMouseButtons[b]) && !s_mousePrevious[b]; }
bool Input::MouseButtonUp  (int b) { return !(s_mouseCurrent[b] || s_injectedMouseButtons[b]) && s_mousePrevious[b]; }

math::Vector2 Input::MousePosition() { return s_mousePos; }
math::Vector2 Input::MouseDelta()
{
    if (s_hasOverrideMouseDelta)
        return s_overrideMouseDelta;
    return { s_mousePos.x - s_prevMousePos.x, s_mousePos.y - s_prevMousePos.y };
}

void Input::OverrideMouseDelta(const math::Vector2& delta)
{
    s_overrideMouseDelta = delta;
    s_hasOverrideMouseDelta = true;
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

void Input::HandleTextInput(wchar_t character)
{
    if (character < 0x20 && character != L'\n' && character != L'\r' && character != L'\b')
        return;
    if (character == L'\b') {
        s_textInput.push_back('\b');
        return;
    }

    char buffer[4] = {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, &character, 1, buffer,
                                           static_cast<int>(sizeof(buffer)), nullptr, nullptr);
    if (count > 0)
        s_textInput.append(buffer, static_cast<size_t>(count));
}

std::string_view Input::TextInput()
{
    return s_textInput;
}

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

bool Input::InjectKey(uint32_t virtualKey, bool pressed)
{
    if (virtualKey >= static_cast<uint32_t>(KEY_COUNT)) return false;
    s_injectedKeys[virtualKey] = pressed;
    return true;
}

bool Input::InjectMouseButton(int button, bool pressed)
{
    if (button < 0 || button >= static_cast<int>(s_mouseCurrent.size())) return false;
    const size_t index = static_cast<size_t>(button);
    s_injectedMouseButtons[index] = pressed;
    const uint32_t virtualKey = button == 0 ? VK_LBUTTON : (button == 1 ? VK_RBUTTON : VK_MBUTTON);
    return InjectKey(virtualKey, pressed);
}

void Input::InjectMousePosition(const math::Vector2& position)
{
    s_mousePos = position;
}

void Input::InjectMouseDelta(const math::Vector2& delta)
{
    OverrideMouseDelta(delta);
}

void Input::InjectMouseScroll(float delta)
{
    s_scrollDelta += delta;
}

bool Input::SetVirtualAxis(std::string_view name, float value)
{
    if (name.empty() || name.size() > 64 || !std::isfinite(value)) return false;
    s_virtualAxes[std::string(name)] = std::clamp(value, -1.0f, 1.0f);
    return true;
}

float Input::GetVirtualAxis(std::string_view name)
{
    const auto iterator = s_virtualAxes.find(std::string(name));
    return iterator != s_virtualAxes.end() ? iterator->second : 0.0f;
}

bool Input::SetVirtualButton(std::string_view name, bool pressed)
{
    if (name.empty() || name.size() > 64) return false;
    s_virtualButtons[std::string(name)] = pressed;
    return true;
}

bool Input::GetVirtualButton(std::string_view name)
{
    const auto iterator = s_virtualButtons.find(std::string(name));
    return iterator != s_virtualButtons.end() && iterator->second;
}

bool Input::GetVirtualButtonDown(std::string_view name)
{
    const bool current = GetVirtualButton(name);
    const auto iterator = s_previousVirtualButtons.find(std::string(name));
    const bool previous = iterator != s_previousVirtualButtons.end() && iterator->second;
    return current && !previous;
}

bool Input::GetVirtualButtonUp(std::string_view name)
{
    const bool current = GetVirtualButton(name);
    const auto iterator = s_previousVirtualButtons.find(std::string(name));
    const bool previous = iterator != s_previousVirtualButtons.end() && iterator->second;
    return !current && previous;
}

void Input::ClearInjected()
{
    s_injectedKeys.fill(false);
    s_injectedMouseButtons.fill(false);
    s_virtualAxes.clear();
    s_virtualButtons.clear();
    s_overrideMouseDelta = {};
    s_scrollDelta = 0.0f;
    s_hasOverrideMouseDelta = false;
    s_textInput.clear();
}

} // namespace fbzz::input
