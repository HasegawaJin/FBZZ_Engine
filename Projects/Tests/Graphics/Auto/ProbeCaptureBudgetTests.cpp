/// @file    ProbeCaptureBudgetTests.cpp
/// @brief   Frame-shared probe attempt limits, reset epochs and fair owner continuation.
/// @author  Hasegawa Jin
/// @date    2026-10-02
#include <TestKit/TestKit.hpp>
#include <Graphics/Renderer/ProbeCaptureBudget.hpp>

namespace fbzz::tests {
namespace {

class ProbeCaptureBudgetTest : public testkit::Fixture {
protected:
    renderer::ProbeCaptureBudget m_budget;
};

TEST_F(ProbeCaptureBudgetTest, ViewsAndScenesShareTheSamePhysicalFrameLimit)
{
    EXPECT_TRUE(m_budget.CanAttempt(0, 7, 1));
    m_budget.MarkAttempt(10, 1, 3);
    EXPECT_FALSE(m_budget.CanAttempt(0, 7, 1));
    EXPECT_TRUE(m_budget.IsAfterCursor(11, 0, 1));
    EXPECT_FALSE(m_budget.CanAttempt(0, 7, 1));
    EXPECT_TRUE(m_budget.CanAttempt(1, 7, 1));
    m_budget.MarkAttempt(11, 0, 1);
    EXPECT_FALSE(m_budget.CanAttempt(1, 7, 1));
}

TEST_F(ProbeCaptureBudgetTest, FailedAttemptsAdvanceTheCursorAndSpendTheSlot)
{
    EXPECT_TRUE(m_budget.IsAfterCursor(10, 0, 0));
    ASSERT_TRUE(m_budget.CanAttempt(100, 7, 2));
    m_budget.MarkAttempt(10, 2, 3);
    EXPECT_FALSE(m_budget.IsAfterCursor(10, 1, 9));
    EXPECT_FALSE(m_budget.IsAfterCursor(10, 2, 3));
    EXPECT_TRUE(m_budget.IsAfterCursor(10, 2, 4));
    EXPECT_TRUE(m_budget.IsAfterCursor(10, 3, 1));
    ASSERT_TRUE(m_budget.CanAttempt(100, 7, 2));
    m_budget.MarkAttempt(10, 3, 1);
    EXPECT_FALSE(m_budget.CanAttempt(100, 7, 2));
    EXPECT_TRUE(m_budget.CanAttempt(101, 7, 2));
    EXPECT_FALSE(m_budget.IsAfterCursor(10, 3, 1));
}

TEST_F(ProbeCaptureBudgetTest, ResourceEpochChangeAndFrameRewindDoNotKeepSpentSlots)
{
    ASSERT_TRUE(m_budget.CanAttempt(100, 7, 1));
    m_budget.MarkAttempt(10, 1, 1);
    EXPECT_FALSE(m_budget.CanAttempt(100, 7, 1));
    EXPECT_TRUE(m_budget.CanAttempt(100, 8, 1));
    m_budget.MarkAttempt(10, 2, 1);
    EXPECT_FALSE(m_budget.CanAttempt(100, 8, 1));
    EXPECT_TRUE(m_budget.CanAttempt(1, 8, 1));
}

TEST_F(ProbeCaptureBudgetTest, ZeroDisablesAttemptsAndLoweringLimitCannotRefundWork)
{
    EXPECT_FALSE(m_budget.CanAttempt(1, 7, 0));
    ASSERT_TRUE(m_budget.CanAttempt(1, 7, 2));
    m_budget.MarkAttempt(10, 1, 1);
    EXPECT_FALSE(m_budget.CanAttempt(1, 7, 1));
    EXPECT_FALSE(m_budget.CanAttempt(1, 7, 0));
    EXPECT_TRUE(m_budget.CanAttempt(1, 7, 2));
}

} /// @note namespace
} /// @note namespace fbzz::tests
