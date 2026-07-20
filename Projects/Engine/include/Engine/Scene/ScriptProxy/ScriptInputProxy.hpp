// FBZZ Engine
// ScriptInputProxy.hpp | fbzz::scene
// Script から入力状態を読むためのショートハンド
#pragma once

#include <Math/Vector2.hpp>
#include <cstdint>
#include <string_view>

namespace fbzz::input {
enum class KeyCode : uint32_t;
}

namespace fbzz::scene {

class Script;

enum class MouseBtn : int { Left = 0, Right = 1, Middle = 2 };

struct ScriptInputProxy {
    Script* script = nullptr;

    bool GetKey(input::KeyCode key) const;
    bool GetKeyDown(input::KeyCode key) const;
    bool GetKeyUp(input::KeyCode key) const;
    float GetAxis(std::string_view name) const;
    bool GetButton(std::string_view name) const;
    bool GetButtonDown(std::string_view name) const;
    bool GetButtonUp(std::string_view name) const;
    math::Vector2 GetMouseDelta() const;
    math::Vector2 GetMousePosition() const;
    float GetMouseScrollDelta() const;
    bool MouseButton(MouseBtn btn) const;
    bool MouseButtonDown(MouseBtn btn) const;
    bool MouseButtonUp(MouseBtn btn) const;
    [[deprecated("Use MouseBtn enum")]] bool MouseButton(int button) const;
    [[deprecated("Use MouseBtn enum")]] bool MouseButtonDown(int button) const;
    [[deprecated("Use MouseBtn enum")]] bool MouseButtonUp(int button) const;
};

} // namespace fbzz::scene
