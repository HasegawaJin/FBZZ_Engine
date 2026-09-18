/// @file    TimeLockstepTests.cpp
/// @brief   ロックステップ中の Time::Tick が実時間に依らず固定 dt で進むことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Core/Time.hpp>

namespace fbzz::tests {

class TimeLockstepTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        fbzz::Time::SetTargetFps(0);
        fbzz::Time::SetTimeScale(1.0f);
        fbzz::Time::SetLockstepDelta(0.0f);
        fbzz::Time::Reset();
        /// @note 最初の Tick は基準時刻を取るだけで dt を作らない。
        fbzz::Time::Tick();
    }

    void TearDown() override
    {
        fbzz::Time::SetLockstepDelta(0.0f);
        fbzz::Time::SetTimeScale(1.0f);
        fbzz::Time::Reset();
        /// @note Reset() は dt を消さない。固定 dt を残すと後続の TimeTickTest が «Reset 後は 0» を観測できない。
        fbzz::Time::deltaTime = 0.0f;
        fbzz::Time::unscaledDeltaTime = 0.0f;
        EngineFixture::TearDown();
    }
};

TEST_F(TimeLockstepTest, EveryTickAdvancesByTheFixedDelta)
{
    fbzz::Time::SetLockstepDelta(0.02f);
    const float start = fbzz::Time::unscaledTime;

    for (int frame = 0; frame < 10; ++frame) fbzz::Time::Tick();

    EXPECT_FLOAT_EQ(fbzz::Time::unscaledDeltaTime, 0.02f);
    EXPECT_NEAR(fbzz::Time::unscaledTime - start, 0.2f, 1e-5f);
}

TEST_F(TimeLockstepTest, TimeScaleStillAppliesOnTopOfTheLockstep)
{
    fbzz::Time::SetLockstepDelta(0.02f);
    fbzz::Time::SetTimeScale(0.5f);

    fbzz::Time::Tick();

    EXPECT_FLOAT_EQ(fbzz::Time::deltaTime, 0.01f);
    EXPECT_FLOAT_EQ(fbzz::Time::unscaledDeltaTime, 0.02f);
}

TEST_F(TimeLockstepTest, ZeroOrNegativeReleasesTheLockstep)
{
    fbzz::Time::SetLockstepDelta(0.02f);
    fbzz::Time::SetLockstepDelta(-1.0f);
    EXPECT_FLOAT_EQ(fbzz::Time::GetLockstepDelta(), 0.0f);
}

TEST_F(TimeLockstepTest, ClampsAbsurdlyLargeSteps)
{
    fbzz::Time::SetLockstepDelta(10.0f);
    EXPECT_LE(fbzz::Time::GetLockstepDelta(), 0.25f);
}

} // namespace fbzz::tests
