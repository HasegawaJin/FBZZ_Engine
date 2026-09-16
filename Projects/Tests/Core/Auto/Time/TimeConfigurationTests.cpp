/// @file    TimeConfigurationTests.cpp
/// @brief   Time の時間倍率・FPS 設定のクランプ契約を自動検証する。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Core/Time.hpp>

namespace fbzz::tests {

class TimeConfigurationTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        fbzz::Time::Reset();
        fbzz::Time::SetTimeScale(1.0f);
        fbzz::Time::SetTargetFps(0);
    }

    void TearDown() override
    {
        fbzz::Time::Reset();
        fbzz::Time::SetTimeScale(1.0f);
        fbzz::Time::SetTargetFps(0);
        EngineFixture::TearDown();
    }
};

TEST_F(TimeConfigurationTest, ClampsNegativeTimeScaleToZero)
{
    fbzz::Time::SetTimeScale(-1.0f);
    EXPECT_FLOAT_EQ(fbzz::Time::GetTimeScale(), 0.0f);
}

TEST_F(TimeConfigurationTest, KeepsPositiveTimeScale)
{
    fbzz::Time::SetTimeScale(0.5f);
    EXPECT_FLOAT_EQ(fbzz::Time::GetTimeScale(), 0.5f);
}

TEST_F(TimeConfigurationTest, DisablesNonPositiveTargetFps)
{
    fbzz::Time::SetTargetFps(-1);
    EXPECT_EQ(fbzz::Time::GetTargetFps(), 0);
    fbzz::Time::SetTargetFps(0);
    EXPECT_EQ(fbzz::Time::GetTargetFps(), 0);
}

TEST_F(TimeConfigurationTest, ClampsPositiveTargetFpsToThirtyOrMore)
{
    fbzz::Time::SetTargetFps(1);
    EXPECT_EQ(fbzz::Time::GetTargetFps(), 30);
    fbzz::Time::SetTargetFps(120);
    EXPECT_EQ(fbzz::Time::GetTargetFps(), 120);
}

} // namespace fbzz::tests
