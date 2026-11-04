/// @file    GameInputConsumption.hpp
/// @brief   Consumes only controls held during game input acquisition until their individual release.
/// @author  Hasegawa Jin
/// @date    2026-10-23
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Input/Gamepad.hpp>
#include <Engine/Core/Cursor.hpp>
#include <cstdint>
#include <array>
#include <cstddef>

namespace fbzz::input::input_detail {

class GameInputConsumption final {
public:
    static constexpr std::size_t KEY_COUNT = 256;
    static constexpr std::size_t MOUSE_COUNT = 3;
    static constexpr std::size_t PAD_COUNT = 4;
    static constexpr std::size_t PAD_BUTTON_COUNT = 16;
    static constexpr std::size_t PAD_AXIS_COUNT = 6;

    /// @note Explicit input.inject automation remains usable without OS focus; physical host input stays excluded.
    [[nodiscard]] static bool ResolveButton(bool physical, bool injected, bool gameOwned, bool consumed)
    { return injected || (gameOwned && !consumed && physical); }

    /// @note Caller viewport coordinates are physical; client-space injection cannot enable them while Host owns input.
    [[nodiscard]] static bool RuntimePointerAvailable(bool gameOwned, bool acquisitionConsumed)
    { return gameOwned && !acquisitionConsumed; }

    void Clear()
    {
        const auto nextRevision = m_interactionRevision + 1;
        *this = {};
        m_interactionRevision = nextRevision;
    }
    void CancelInteractions() { ++m_interactionRevision; }
    [[nodiscard]] std::uint64_t InteractionRevision() const { return m_interactionRevision; }
    void BeginFrame() { m_motionBlocked = false; }
    void BlockMotion() { m_motionBlocked = true; }
    [[nodiscard]] bool MotionBlocked() const { return m_motionBlocked; }
    void BlockKey(std::size_t key, bool held) { if (key < KEY_COUNT) m_keys[key] = held; }
    void BlockMouse(std::size_t button, bool held) { if (button < MOUSE_COUNT) m_mouse[button] = held; }
    void BlockPadButton(std::size_t pad, std::size_t button, bool held)
    { if (pad < PAD_COUNT && button < PAD_BUTTON_COUNT) m_padButtons[pad][button] = held; }
    void BlockPadAxis(std::size_t pad, std::size_t axis, bool held)
    { if (pad < PAD_COUNT && axis < PAD_AXIS_COUNT) m_padAxes[pad][axis] = held; }
    void ReleaseKey(std::size_t key, bool held) { if (key < KEY_COUNT && !held) m_keys[key] = false; }
    void ReleaseMouse(std::size_t button, bool held) { if (button < MOUSE_COUNT && !held) m_mouse[button] = false; }
    void ReleasePadButton(std::size_t pad, std::size_t button, bool held)
    { if (pad < PAD_COUNT && button < PAD_BUTTON_COUNT && !held) m_padButtons[pad][button] = false; }
    void ReleasePadAxis(std::size_t pad, std::size_t axis, bool held)
    { if (pad < PAD_COUNT && axis < PAD_AXIS_COUNT && !held) m_padAxes[pad][axis] = false; }
    [[nodiscard]] bool KeyBlocked(std::size_t key) const { return key < KEY_COUNT && m_keys[key]; }
    [[nodiscard]] bool MouseBlocked(std::size_t button) const { return button < MOUSE_COUNT && m_mouse[button]; }
    [[nodiscard]] bool PadButtonBlocked(std::size_t pad, std::size_t button) const
    { return pad < PAD_COUNT && button < PAD_BUTTON_COUNT && m_padButtons[pad][button]; }
    [[nodiscard]] bool PadAxisBlocked(std::size_t pad, std::size_t axis) const
    { return pad < PAD_COUNT && axis < PAD_AXIS_COUNT && m_padAxes[pad][axis]; }

private:
    std::array<bool, KEY_COUNT> m_keys{};
    std::array<bool, MOUSE_COUNT> m_mouse{};
    std::array<std::array<bool, PAD_BUTTON_COUNT>, PAD_COUNT> m_padButtons{};
    std::array<std::array<bool, PAD_AXIS_COUNT>, PAD_COUNT> m_padAxes{};
    bool m_motionBlocked = false;
    std::uint64_t m_interactionRevision = 0;
};

/// @note Engine-private shared consumption state; the raw Input API stays available to Editor UI.
FBZZ_ENGINE_API GameInputConsumption& GameInputConsumptionState();

/// @note Private gameplay/host source selection shared by scripts, action maps and runtime UI.
struct GameInputAccess final {
    static bool KeyHeld(KeyCode key) { return Input::GameKeyHeld(key); }
    static bool MouseButton(int button) { return Input::GameMouseButton(button); }
    static math::Vector2 MouseDelta() { return Input::GameMouseDelta(); }
    static float MouseScrollDelta() { return Input::GameMouseScrollDelta(); }
    static std::string_view TextInput() { return Input::GameTextInput(); }
    static std::uint64_t InteractionRevision() { return GameInputConsumptionState().InteractionRevision(); }
    static bool InjectedKeyPending(KeyCode key)
    {
        const auto index = static_cast<std::size_t>(key);
        return index < Input::KEY_COUNT && (Input::s_injectedKeys[index] || Input::s_previousInjectedKeys[index]);
    }
    static bool PointerAvailable()
    {
        const auto& consumed = GameInputConsumptionState();
        return GameInputConsumption::RuntimePointerAvailable(core::Cursor::CanReadGameInput(), consumed.MotionBlocked());
    }
    static bool PointerPressed(bool viewportPressed)
    {
        const auto& consumed = GameInputConsumptionState();
        return GameInputConsumption::ResolveButton(viewportPressed, Input::s_injectedMouseButtons[0],
            core::Cursor::CanReadGameInput(), consumed.MouseBlocked(0) || consumed.MotionBlocked());
    }
    static bool PadButtonHeld(GamepadButton button, int pad)
    {
        return core::Cursor::CanReadGameInput()
            && !GameInputConsumptionState().PadButtonBlocked(pad, static_cast<std::size_t>(button))
            && Gamepad::ButtonHeld(button, pad);
    }
    static float PadAxis(GamepadAxis axis, int pad)
    {
        return core::Cursor::CanReadGameInput()
            && !GameInputConsumptionState().PadAxisBlocked(pad, static_cast<std::size_t>(axis))
            ? Gamepad::Axis(axis, pad) : 0.0f;
    }
};

} /// @note namespace fbzz::input::input_detail
