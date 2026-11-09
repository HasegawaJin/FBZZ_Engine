/// @file    CursorControlPolicyTests.cpp
/// @brief   Verifies cursor ownership lifecycles and per-control acquisition consumption without OS interaction.
/// @author  Hasegawa Jin
/// @date    2026-10-23
#include <TestKit/TestKit.hpp>
#include "../../../Engine/src/Core/Platform/CursorControlPolicy.hpp"
#include "../../../Engine/src/Input/GameInputConsumption.hpp"

namespace fbzz::tests {

class CursorControlPolicyTest : public testkit::Fixture {
protected:
    core::cursor_detail::CursorControlPolicy m_control;
    const core::CursorPolicy m_locked{core::CursorLockMode::Locked, true};
};

TEST_F(CursorControlPolicyTest, LockedHidesEffectiveCursorWithoutChangingRequestedVisibility)
{
    const auto state = m_control.Resolve(m_locked, false);
    EXPECT_TRUE(state.requested.visible);
    EXPECT_FALSE(state.effective.visible);
    EXPECT_EQ(state.effective.lockMode, core::CursorLockMode::Locked);
    EXPECT_EQ(state.inputOwner, core::CursorInputOwner::GAME);
}

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

TEST_F(CursorControlPolicyTest, CursorOnlyOverrideDoesNotTakeGameInputOwnership)
{
    const auto state = m_control.Resolve(m_locked, true);
    EXPECT_EQ(state.inputOwner, core::CursorInputOwner::GAME);
    EXPECT_EQ(state.suspendedReason, core::CursorSuspendReason::HOST_OVERRIDE);
    EXPECT_TRUE(state.effective.visible);
    EXPECT_EQ(state.effective.lockMode, core::CursorLockMode::None);
}

TEST_F(CursorControlPolicyTest, MissingTargetRestoresVisibleFreeCursor)
{
    const auto state = m_control.Resolve(m_locked, false, false);
    EXPECT_EQ(state.suspendedReason, core::CursorSuspendReason::TARGET_UNAVAILABLE);
    EXPECT_TRUE(state.effective.visible);
    EXPECT_EQ(state.effective.lockMode, core::CursorLockMode::None);
}

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

TEST_F(GameInputConsumptionTest, AcquisitionClickRemainsConsumedThroughReleaseFrame)
{
    m_consumption.BlockMouse(0, true);
    m_consumption.ReleaseMouse(0, true);
    EXPECT_TRUE(m_consumption.MouseBlocked(0));
    EXPECT_FALSE(m_consumption.MouseBlocked(1));
    m_consumption.ReleaseMouse(0, false);
    EXPECT_FALSE(m_consumption.MouseBlocked(0));
}

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
