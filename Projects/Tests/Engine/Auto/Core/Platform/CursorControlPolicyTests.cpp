/// @file    CursorControlPolicyTests.cpp
/// @brief   Verifies cursor ownership lifecycles and per-control acquisition consumption without OS interaction.
/// @author  Hasegawa Jin
/// @date    2026-10-23
#include <TestKit/TestKit.hpp>
#include "../../../../../Engine/src/Core/Platform/CursorControlPolicy.hpp"
#include "../../../../../Engine/src/Core/Platform/LockedMouseMotion.hpp"
#include "../../../../../Engine/src/Input/GameInputConsumption.hpp"

namespace fbzz::tests {

class CursorControlPolicyTest : public testkit::Fixture {
protected:
    core::cursor_detail::CursorControlPolicy m_control;
    const core::CursorPolicy m_locked{core::CursorLockMode::Locked, true};
};

/// @brief Locked hides effective cursor without changing requested visibility.
TEST_F(CursorControlPolicyTest, LockedHidesEffectiveCursorWithoutChangingRequestedVisibility)
{
    const auto state = m_control.Resolve(m_locked, false);
    EXPECT_TRUE(state.requested.visible);
    EXPECT_FALSE(state.effective.visible);
    EXPECT_EQ(state.effective.lockMode, core::CursorLockMode::Locked);
    EXPECT_EQ(state.inputOwner, core::CursorInputOwner::GAME);
}

/// @brief Locked mouse accumulates relative packets once per frame.
TEST_F(CursorControlPolicyTest, LockedMouseAccumulatesRelativePacketsOncePerFrame)
{
    core::cursor_detail::LockedMouseMotion motion;
    motion.Add(17, -9);
    motion.Add(-5, 2);
    const auto delta = motion.Take();
    ASSERT_TRUE(delta);
    EXPECT_FLOAT_EQ(delta->x, 12.0f);
    EXPECT_FLOAT_EQ(delta->y, -7.0f);
    EXPECT_FALSE(motion.Take());
}

/// @brief Locked mouse distinguishes cancelled motion from no relative packets.
TEST_F(CursorControlPolicyTest, LockedMouseDistinguishesCancelledMotionFromNoRelativePackets)
{
    core::cursor_detail::LockedMouseMotion motion;
    motion.Add(0, 0);
    EXPECT_FALSE(motion.Take());
    motion.Add(12, 4);
    motion.Add(-12, -4);
    const auto delta = motion.Take();
    ASSERT_TRUE(delta);
    EXPECT_FLOAT_EQ(delta->x, 0.0f);
    EXPECT_FLOAT_EQ(delta->y, 0.0f);
}

/// @brief Visibility and movement restriction do not decide input ownership.
TEST_F(CursorControlPolicyTest, VisibilityAndMovementRestrictionDoNotDecideInputOwnership)
{
    for (auto mode : {core::CursorLockMode::None, core::CursorLockMode::Confined, core::CursorLockMode::Locked}) {
        for (bool visible : {true, false}) {
            const auto state = m_control.Resolve({mode, visible}, false);
            EXPECT_EQ(state.inputOwner, core::CursorInputOwner::GAME);
            EXPECT_EQ(state.effective.lockMode, mode);
            EXPECT_EQ(state.effective.visible, mode == core::CursorLockMode::Locked ? false : visible);
        }
    }
}

/// @brief Focus return requires explicit acquisition.
TEST_F(CursorControlPolicyTest, FocusReturnRequiresExplicitAcquisition)
{
    m_control.SetWindowActive(false);
    EXPECT_FALSE(m_control.Acquire());
    m_control.SetWindowActive(true);
    auto state = m_control.Resolve(m_locked, false);
    EXPECT_EQ(state.inputOwner, core::CursorInputOwner::HOST);
    EXPECT_TRUE(state.effective.visible);
    EXPECT_EQ(state.effective.lockMode, core::CursorLockMode::None);
    EXPECT_EQ(state.suspendedReason, core::CursorSuspendReason::WINDOW_INACTIVE);
    EXPECT_TRUE(m_control.Acquire());
    EXPECT_EQ(m_control.Resolve(m_locked, false).effective.lockMode, core::CursorLockMode::Locked);
}

/// @brief Release pause view loss and window change preserve request until explicit click.
TEST_F(CursorControlPolicyTest, ReleasePauseViewLossAndWindowChangePreserveRequestUntilExplicitClick)
{
    for (auto reason : {core::CursorSuspendReason::RELEASED, core::CursorSuspendReason::PAUSED,
         core::CursorSuspendReason::GAME_VIEW_UNFOCUSED, core::CursorSuspendReason::WINDOW_MOVING,
         core::CursorSuspendReason::NOT_PLAYING, core::CursorSuspendReason::TARGET_UNAVAILABLE}) {
        m_control.Release(reason);
        const auto state = m_control.Resolve(m_locked, false);
        EXPECT_EQ(state.suspendedReason, reason);
        EXPECT_EQ(state.inputOwner, core::CursorInputOwner::HOST);
        EXPECT_TRUE(state.effective.visible);
        EXPECT_EQ(state.effective.lockMode, core::CursorLockMode::None);
        EXPECT_EQ(state.requested.lockMode, core::CursorLockMode::Locked);
        EXPECT_TRUE(state.requested.visible);
        EXPECT_TRUE(m_control.Acquire());
    }
}

/// @brief Cursor only override does not take game input ownership.
TEST_F(CursorControlPolicyTest, CursorOnlyOverrideDoesNotTakeGameInputOwnership)
{
    const auto state = m_control.Resolve(m_locked, true);
    EXPECT_EQ(state.inputOwner, core::CursorInputOwner::GAME);
    EXPECT_EQ(state.suspendedReason, core::CursorSuspendReason::HOST_OVERRIDE);
    EXPECT_TRUE(state.effective.visible);
    EXPECT_EQ(state.effective.lockMode, core::CursorLockMode::None);
}

/// @brief Missing target restores visible free cursor.
TEST_F(CursorControlPolicyTest, MissingTargetRestoresVisibleFreeCursor)
{
    const auto state = m_control.Resolve(m_locked, false, false);
    EXPECT_EQ(state.suspendedReason, core::CursorSuspendReason::TARGET_UNAVAILABLE);
    EXPECT_TRUE(state.effective.visible);
    EXPECT_EQ(state.effective.lockMode, core::CursorLockMode::None);
}

/// @brief Automation denies capture even after focus changes or explicit acquire.
TEST_F(CursorControlPolicyTest, AutomationDeniesCaptureEvenAfterFocusChangesOrExplicitAcquire)
{
    m_control.SetPhysicalInputAllowed(false);
    EXPECT_FALSE(m_control.IsPhysicalInputAllowed());
    for (bool active : {true, false, true}) {
        m_control.SetWindowActive(active);
        EXPECT_FALSE(m_control.Acquire());
        const auto state = m_control.Resolve(m_locked, false);
        EXPECT_EQ(state.requested.lockMode, core::CursorLockMode::Locked);
        EXPECT_EQ(state.effective.lockMode, core::CursorLockMode::None);
        EXPECT_TRUE(state.effective.visible);
        EXPECT_EQ(state.inputOwner, core::CursorInputOwner::HOST);
        EXPECT_EQ(state.suspendedReason, core::CursorSuspendReason::HOST_OVERRIDE);
    }
    m_control.Release(core::CursorSuspendReason::NOT_PLAYING);
    EXPECT_FALSE(m_control.Acquire());
}

/// @brief Manual play after automation requires fresh explicit acquisition.
TEST_F(CursorControlPolicyTest, ManualPlayAfterAutomationRequiresFreshExplicitAcquisition)
{
    m_control.SetPhysicalInputAllowed(false);
    m_control.SetPhysicalInputAllowed(true);
    EXPECT_TRUE(m_control.IsPhysicalInputAllowed());
    EXPECT_FALSE(m_control.OwnsGameInput());
    EXPECT_TRUE(m_control.Acquire());
    EXPECT_EQ(m_control.Resolve(m_locked, false).effective.lockMode, core::CursorLockMode::Locked);
    m_control.SetPhysicalInputAllowed(false);
    EXPECT_FALSE(m_control.OwnsGameInput());
}

/// @brief Automation preserves injected buttons without physical input.
TEST_F(CursorControlPolicyTest, AutomationPreservesInjectedButtonsWithoutPhysicalInput)
{
    using Filter = input::input_detail::GameInputConsumption;
    m_control.SetPhysicalInputAllowed(false);
    EXPECT_FALSE(Filter::ResolveButton(true, false, m_control.OwnsGameInput(), false));
    EXPECT_TRUE(Filter::ResolveButton(false, true, m_control.OwnsGameInput(), false));
    EXPECT_TRUE(Filter::ResolveButton(true, true, m_control.OwnsGameInput(), true));
}

class GameInputConsumptionTest : public testkit::Fixture {
protected:
    input::input_detail::GameInputConsumption m_consumption;
};

/// @brief Acquisition click remains consumed through release frame.
TEST_F(GameInputConsumptionTest, AcquisitionClickRemainsConsumedThroughReleaseFrame)
{
    m_consumption.BlockMouse(0, true);
    m_consumption.ReleaseMouse(0, true);
    EXPECT_TRUE(m_consumption.MouseBlocked(0));
    EXPECT_FALSE(m_consumption.MouseBlocked(1));
    m_consumption.ReleaseMouse(0, false);
    EXPECT_FALSE(m_consumption.MouseBlocked(0));
}

/// @brief Held host controls do not block fresh unrelated controls.
TEST_F(GameInputConsumptionTest, HeldHostControlsDoNotBlockFreshUnrelatedControls)
{
    m_consumption.BlockKey(65, true);
    m_consumption.BlockPadAxis(0, 0, true);
    EXPECT_TRUE(m_consumption.KeyBlocked(65));
    EXPECT_FALSE(m_consumption.KeyBlocked(87));
    EXPECT_FALSE(m_consumption.PadAxisBlocked(0, 1));
    m_consumption.ReleasePadAxis(0, 0, false);
    EXPECT_FALSE(m_consumption.PadAxisBlocked(0, 0));
    EXPECT_TRUE(m_consumption.KeyBlocked(65));
}

/// @brief Checks motion consumption lasts one frame without changing buttons.
TEST_F(GameInputConsumptionTest, MotionConsumptionLastsOneFrameWithoutChangingButtons)
{
    m_consumption.BlockMotion();
    m_consumption.BlockMouse(0, true);
    EXPECT_TRUE(m_consumption.MotionBlocked());
    m_consumption.BeginFrame();
    EXPECT_FALSE(m_consumption.MotionBlocked());
    EXPECT_TRUE(m_consumption.MouseBlocked(0));
    m_consumption.Clear();
    EXPECT_FALSE(m_consumption.MouseBlocked(0));
}

/// @brief Hidden automation uses explicit injection without owning physical input.
TEST_F(GameInputConsumptionTest, HiddenAutomationUsesExplicitInjectionWithoutOwningPhysicalInput)
{
    using Filter = input::input_detail::GameInputConsumption;
    EXPECT_FALSE(Filter::ResolveButton(true, false, false, false));
    EXPECT_TRUE(Filter::ResolveButton(false, true, false, false));
    EXPECT_TRUE(Filter::ResolveButton(true, true, false, true));
    EXPECT_FALSE(Filter::ResolveButton(true, false, true, true));
    EXPECT_TRUE(Filter::ResolveButton(true, false, true, false));
}

} /// @note namespace fbzz::tests
