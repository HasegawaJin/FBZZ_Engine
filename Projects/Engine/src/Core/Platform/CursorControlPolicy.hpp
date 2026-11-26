/// @file    CursorControlPolicy.hpp
/// @brief   OS を呼ばずに、要求されたカーソル設定と明示的なゲーム入力の所有権を解決する。
/// @author  Hasegawa Jin
/// @date    2026-10-23
#pragma once
#include <Engine/Core/Platform/Cursor.hpp>

namespace fbzz::core::cursor_detail {

class CursorControlPolicy final {
public:
    bool Acquire()
    {
        if (!m_physicalInputAllowed || !m_windowActive) {
            return false;
        }
        m_owner = CursorInputOwner::GAME;
        m_reason = CursorSuspendReason::NONE;
        return true;
    }

    void Release(CursorSuspendReason reason)
    {
        m_owner = CursorInputOwner::HOST;
        m_reason = reason == CursorSuspendReason::NONE ? CursorSuspendReason::RELEASED : reason;
    }

    void SetPhysicalInputAllowed(bool allowed)
    {
        m_physicalInputAllowed = allowed;
        if (!allowed) {
            Release(CursorSuspendReason::HOST_OVERRIDE);
        }
    }

    [[nodiscard]] bool IsPhysicalInputAllowed() const { return m_physicalInputAllowed; }

    void SetWindowActive(bool active)
    {
        m_windowActive = active;
        if (!active) {
            Release(CursorSuspendReason::WINDOW_INACTIVE);
        }
    }

    [[nodiscard]] bool OwnsGameInput() const
    {
        return m_physicalInputAllowed && m_windowActive && m_owner == CursorInputOwner::GAME;
    }

    /// @note フォーカスの復帰だけでは取得しない。所有権の取得には明示的な Acquire が必要。
    /// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/playing-and-simulating-in-unreal-engine Play In Editor のマウス操作。
    [[nodiscard]] CursorState Resolve(CursorPolicy requested, bool suppressed, bool targetAvailable = true) const
    {
        CursorState state{};
        state.requested = requested;
        state.windowActive = m_windowActive;
        state.inputOwner = OwnsGameInput() ? CursorInputOwner::GAME : CursorInputOwner::HOST;
        state.suspendedReason = !m_physicalInputAllowed ? CursorSuspendReason::HOST_OVERRIDE
            : !m_windowActive ? CursorSuspendReason::WINDOW_INACTIVE
            : !OwnsGameInput() ? m_reason
            : suppressed ? CursorSuspendReason::HOST_OVERRIDE
            : !targetAvailable ? CursorSuspendReason::TARGET_UNAVAILABLE : CursorSuspendReason::NONE;
        if (state.suspendedReason == CursorSuspendReason::NONE) {
            state.effective = requested;
            /// @see https://docs.unity3d.com/ScriptReference/Cursor-lockState.html ロックが実際のカーソル表示状態に優先する規定。
            if (requested.lockMode == CursorLockMode::Locked) {
                state.effective.visible = false;
            }
        }
        return state;
    }

private:
    CursorInputOwner m_owner = CursorInputOwner::GAME; ///< 明示的なホストの引き渡し前は、既定の所有権で従来のスタンドアロン起動時の挙動を保つ。
    CursorSuspendReason m_reason = CursorSuspendReason::NONE;
    bool m_windowActive = true;
    bool m_physicalInputAllowed = true;
};

} // namespace fbzz::core::cursor_detail
