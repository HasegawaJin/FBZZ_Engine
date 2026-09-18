/// @file    InputRecorder.cpp
/// @brief   Play 中の入力の差分記録。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include <Editor/Playtest/InputRecorder.hpp>

#include <Engine/Input/Input.hpp>
#include <Engine/Input/InputActionMap.hpp>
#include <Engine/Input/KeyCode.hpp>

#include <cmath>

namespace fbzz::editor::playtest {

namespace {

using ai::JsonValue;

/// @note 軸は平滑化で毎フレーム少しずつ動く。この差より小さい変化は記録しない (列が肥大しないように)。
constexpr float kAxisEpsilon = 0.01f;

JsonValue Vector2Json(float x, float y)
{
    JsonValue array = JsonValue::MakeArray();
    array.Push(JsonValue(static_cast<double>(x)));
    array.Push(JsonValue(static_cast<double>(y)));
    return array;
}

} // namespace

void InputRecorder::Start()
{
    m_recording = true;
    m_frame = 0;
    m_keys.fill(false);
    m_mouseButtons.fill(false);
    m_actions.clear();
    m_axes.clear();
    m_events = JsonValue::MakeArray();
}

JsonValue InputRecorder::Stop()
{
    JsonValue result = JsonValue::MakeObject();
    result.Set("version", JsonValue(1));
    result.Set("frames", JsonValue(static_cast<double>(m_frame)));
    result.Set("events", m_recording ? std::move(m_events) : JsonValue::MakeArray());
    m_recording = false;
    m_events = JsonValue::MakeArray();
    return result;
}

void InputRecorder::Emit(JsonValue inject)
{
    JsonValue event = JsonValue::MakeObject();
    event.Set("frame", JsonValue(static_cast<double>(m_frame)));
    event.Set("inject", std::move(inject));
    m_events.Push(std::move(event));
}

void InputRecorder::Capture()
{
    if (!m_recording) return;

    for (int key = 1; key < static_cast<int>(m_keys.size()); ++key) {
        const bool held = input::Input::KeyHeld(static_cast<input::KeyCode>(key));
        if (held == m_keys[static_cast<size_t>(key)]) continue;
        m_keys[static_cast<size_t>(key)] = held;
        JsonValue inject = JsonValue::MakeObject();
        inject.Set("kind", JsonValue("key"));
        inject.Set("key", JsonValue("#" + std::to_string(key)));
        inject.Set("pressed", JsonValue(held));
        Emit(std::move(inject));
    }

    for (int button = 0; button < static_cast<int>(m_mouseButtons.size()); ++button) {
        const bool held = input::Input::MouseButton(button);
        if (held == m_mouseButtons[static_cast<size_t>(button)]) continue;
        m_mouseButtons[static_cast<size_t>(button)] = held;
        JsonValue inject = JsonValue::MakeObject();
        inject.Set("kind", JsonValue("mouseButton"));
        inject.Set("button", JsonValue(button));
        inject.Set("pressed", JsonValue(held));
        Emit(std::move(inject));
    }

    /// @note 移動量は «そのフレームだけ» の値なので、差分でなく非ゼロのフレームを全部残す。
    const math::Vector2 delta = input::Input::MouseDelta();
    if (delta.x != 0.0f || delta.y != 0.0f) {
        JsonValue inject = JsonValue::MakeObject();
        inject.Set("kind", JsonValue("mouseDelta"));
        inject.Set("value", Vector2Json(delta.x, delta.y));
        Emit(std::move(inject));
    }
    const float scroll = input::Input::MouseScrollDelta();
    if (scroll != 0.0f) {
        JsonValue inject = JsonValue::MakeObject();
        inject.Set("kind", JsonValue("mouseScroll"));
        inject.Set("value", JsonValue(static_cast<double>(scroll)));
        Emit(std::move(inject));
    }

    for (const input::InputAction& action : input::InputActionMap::GetActions()) {
        const bool held = input::InputActionMap::GetAction(action.name);
        auto iterator = m_actions.find(action.name);
        const bool previous = iterator != m_actions.end() && iterator->second;
        if (held == previous) continue;
        m_actions[action.name] = held;
        JsonValue inject = JsonValue::MakeObject();
        inject.Set("kind", JsonValue("gamepadButton"));
        inject.Set("buttonName", JsonValue(action.name));
        inject.Set("pressed", JsonValue(held));
        Emit(std::move(inject));
    }
    for (const input::InputAxis& axis : input::InputActionMap::GetAxes()) {
        const float value = input::InputActionMap::GetAxis(axis.name);
        auto iterator = m_axes.find(axis.name);
        const float previous = iterator != m_axes.end() ? iterator->second : 0.0f;
        if (std::fabs(value - previous) < kAxisEpsilon && !(value == 0.0f && previous != 0.0f)) continue;
        m_axes[axis.name] = value;
        JsonValue inject = JsonValue::MakeObject();
        inject.Set("kind", JsonValue("axis"));
        inject.Set("axis", JsonValue(axis.name));
        inject.Set("value", JsonValue(static_cast<double>(value)));
        Emit(std::move(inject));
    }

    ++m_frame;
}

} // namespace fbzz::editor::playtest
