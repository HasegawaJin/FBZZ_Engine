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

struct ScriptInputProxy {
    Script* script = nullptr;

    bool GetKey(input::KeyCode key) const;
    bool GetKeyDown(input::KeyCode key) const;
    bool GetKeyUp(input::KeyCode key) const;
    float GetAxis(std::string_view name) const;
    math::Vector2 GetMouseDelta() const;
    math::Vector2 GetMousePosition() const;
    float GetMouseScrollDelta() const;
    bool MouseButton(int button) const;
    bool MouseButtonDown(int button) const;
    bool MouseButtonUp(int button) const;
};

} // namespace fbzz::scene
