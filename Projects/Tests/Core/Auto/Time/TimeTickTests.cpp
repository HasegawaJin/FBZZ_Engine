/// @file    TimeTickTests.cpp
/// @brief   Time のリセットとフレーム進行を実時間に依存しない範囲で自動検証する。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Core/Timing/Time.hpp>

namespace fbzz::tests {

class TimeTickTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        fbzz::Time::SetTargetFps(0);
        fbzz::Time::SetTimeScale(1.0f);
        fbzz::Time::Reset();
    }

    void TearDown() override
    {
        fbzz::Time::Reset();
        fbzz::Time::SetTimeScale(1.0f);
        fbzz::Time::SetTargetFps(0);
        EngineFixture::TearDown();
    }
};

/// @brief Reset clears accumulated state.
TEST_F(TimeTickTest, ResetClearsAccumulatedState)
{
    fbzz::Time::Tick();
    fbzz::Time::Reset();

    /// @note dt は «累積» ではないので Reset() は消さない。プロセス最初の Tick でだけ 0 になるため、順序に依存させない。
    EXPECT_FLOAT_EQ(fbzz::Time::time, 0.0f);
    EXPECT_FLOAT_EQ(fbzz::Time::unscaledTime, 0.0f);
    EXPECT_EQ(fbzz::Time::frameCount, 0u);
}

/// @brief Tick advances the frame counter.
TEST_F(TimeTickTest, TickAdvancesTheFrameCounter)
{
    fbzz::Time::Tick();
    fbzz::Time::Tick();

    EXPECT_EQ(fbzz::Time::frameCount, 2u);
    EXPECT_GE(fbzz::Time::deltaTime, 0.0f);
    EXPECT_GE(fbzz::Time::unscaledDeltaTime, 0.0f);
}

/// @brief Zero time scale freezes scaled time but keeps ticking.
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
