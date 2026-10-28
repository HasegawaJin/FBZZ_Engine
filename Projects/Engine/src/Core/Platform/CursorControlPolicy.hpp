/// @file    CursorControlPolicy.hpp
/// @brief   Resolves requested cursor settings and explicit game input ownership without OS calls.
/// @author  Hasegawa Jin
/// @date    2026-10-23
#pragma once
#include <Engine/Core/Cursor.hpp>

namespace fbzz::core::cursor_detail {

class CursorControlPolicy final {
public:
    bool Acquire()
    {
        if (!m_windowActive) return false;
        m_owner = CursorInputOwner::GAME;
        m_reason = CursorSuspendReason::NONE;
        return true;
    }

    void Release(CursorSuspendReason reason)
    {
        m_owner = CursorInputOwner::HOST;
        m_reason = reason == CursorSuspendReason::NONE ? CursorSuspendReason::RELEASED : reason;
    }

    void SetWindowActive(bool active)
    {
        m_windowActive = active;
        if (!active) Release(CursorSuspendReason::WINDOW_INACTIVE);
    }

    [[nodiscard]] bool OwnsGameInput() const
    {
        return m_windowActive && m_owner == CursorInputOwner::GAME;
    }

    /// @note Returning focus never acquires ownership; only an explicit Acquire does.
    /// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/playing-and-simulating-in-unreal-engine Play In Editor mouse control.
    [[nodiscard]] CursorState Resolve(CursorPolicy requested, bool suppressed, bool targetAvailable = true) const
    {
        CursorState state{};
        state.requested = requested;
        state.windowActive = m_windowActive;
        state.inputOwner = OwnsGameInput() ? CursorInputOwner::GAME : CursorInputOwner::HOST;
        state.suspendedReason = !m_windowActive ? CursorSuspendReason::WINDOW_INACTIVE
            : !OwnsGameInput() ? m_reason
            : suppressed ? CursorSuspendReason::HOST_OVERRIDE
            : !targetAvailable ? CursorSuspendReason::TARGET_UNAVAILABLE : CursorSuspendReason::NONE;
        if (state.suspendedReason == CursorSuspendReason::NONE) {
            state.effective = requested;
            /// @see https://docs.unity3d.com/ScriptReference/Cursor-lockState.html Locked overrides effective visibility.
            if (requested.lockMode == CursorLockMode::Locked) state.effective.visible = false;
        }
        return state;
    }

private:
    /// @note Default ownership preserves existing standalone startup before an explicit host handoff.
    CursorInputOwner m_owner = CursorInputOwner::GAME;
    CursorSuspendReason m_reason = CursorSuspendReason::NONE;
    bool m_windowActive = true;
};

} /// @note namespace fbzz::core::cursor_detail
