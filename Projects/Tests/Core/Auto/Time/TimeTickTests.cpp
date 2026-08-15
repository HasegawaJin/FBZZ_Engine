// FBZZ Engine
// TimeTickTests.cpp | GoogleTest
// Time のリセットとフレーム進行を実時間に依存しない範囲で自動検証する。
#include <gtest/gtest.h>

#include <Engine/Core/Time.hpp>

namespace fbzz::tests {

class TimeTickTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        fbzz::Time::SetTargetFps(0);
        fbzz::Time::SetTimeScale(1.0f);
        fbzz::Time::Reset();
    }

    void TearDown() override
    {
        fbzz::Time::Reset();
        fbzz::Time::SetTimeScale(1.0f);
        fbzz::Time::SetTargetFps(0);
    }
};

TEST_F(TimeTickTest, ResetClearsAccumulatedState)
{
    fbzz::Time::Tick();
    fbzz::Time::Reset();

    EXPECT_FLOAT_EQ(fbzz::Time::time, 0.0f);
    EXPECT_FLOAT_EQ(fbzz::Time::unscaledTime, 0.0f);
    EXPECT_FLOAT_EQ(fbzz::Time::deltaTime, 0.0f);
    EXPECT_FLOAT_EQ(fbzz::Time::unscaledDeltaTime, 0.0f);
    EXPECT_EQ(fbzz::Time::frameCount, 0u);
}

TEST_F(TimeTickTest, TickAdvancesTheFrameCounter)
{
    fbzz::Time::Tick();
    fbzz::Time::Tick();

    EXPECT_EQ(fbzz::Time::frameCount, 2u);
    EXPECT_GE(fbzz::Time::deltaTime, 0.0f);
    EXPECT_GE(fbzz::Time::unscaledDeltaTime, 0.0f);
}

TEST_F(TimeTickTest, ZeroTimeScaleFreezesScaledTimeButKeepsTicking)
{
    fbzz::Time::SetTimeScale(0.0f);
    fbzz::Time::Tick();
    fbzz::Time::Tick();

    EXPECT_FLOAT_EQ(fbzz::Time::deltaTime, 0.0f);
    EXPECT_FLOAT_EQ(fbzz::Time::GetTimeScale(), 0.0f);
    EXPECT_EQ(fbzz::Time::frameCount, 2u);
}

} // namespace fbzz::tests
