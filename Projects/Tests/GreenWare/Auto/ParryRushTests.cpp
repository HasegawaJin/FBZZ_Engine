/// @file    ParryRushTests.cpp
/// @brief   連撃チャンスの時計分離・停止優先・解除と再成功を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-15
#include <TestKit/TestKit.hpp>
#include <TestKit/Deterministic.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Scripts/Game/TimeManagerComponent.hpp>

namespace fbzz::tests {

class ParryRushTest : public testkit::EngineFixture {
protected:
    sandbox::TimeManagerComponent m_manager;
    float m_savedDelta = 0.0f;
    float m_savedUnscaled = 0.0f;
    float m_savedScale = 1.0f;

    void SetUp() override
    {
        EngineFixture::SetUp();
        m_savedDelta = Time::deltaTime;
        m_savedUnscaled = Time::unscaledDeltaTime;
        m_savedScale = Time::timeScale;
        m_manager.OnStart();
    }

    void TearDown() override
    {
        m_manager.OnDestroy();
        Time::deltaTime = m_savedDelta;
        Time::unscaledDeltaTime = m_savedUnscaled;
        Time::timeScale = m_savedScale;
        EngineFixture::TearDown();
    }

    void Step(float realSeconds)
    {
        Time::unscaledDeltaTime = realSeconds;
        Time::deltaTime = realSeconds * Time::timeScale;
        m_manager.OnUpdate();
    }
};

TEST_F(ParryRushTest, PlayerKeepsRealTimeWhileWorldSlows)
{
    m_manager.BeginParryRush(2.0f, 0.25f, 1.65f);
    Step(testkit::kFixedDeltaTime);
    Step(testkit::kFixedDeltaTime);
    EXPECT_NEAR(Time::timeScale, 0.25f, testkit::kTolerance);
    EXPECT_NEAR(sandbox::TimeManagerComponent::PlayerDeltaTime(),
                testkit::kFixedDeltaTime, testkit::kTolerance);
    EXPECT_NEAR(sandbox::TimeManagerComponent::RushAttackSpeed(), 1.65f, testkit::kTolerance);
    EXPECT_NEAR(m_manager.Slow01(), 0.75f, testkit::kTolerance);
}

TEST_F(ParryRushTest, PauseWinsOverHitstopAndPreservesChance)
{
    m_manager.BeginParryRush(2.0f, 0.25f, 1.65f);
    m_manager.SetOverride(0.1f);
    m_manager.SetPaused(true);
    Step(4.0f);
    EXPECT_NEAR(Time::timeScale, 0.0f, testkit::kTolerance);
    EXPECT_NEAR(m_manager.ParryRush01(), 1.0f, testkit::kTolerance);
    EXPECT_NEAR(sandbox::TimeManagerComponent::PlayerDeltaTime(), 0.0f, testkit::kTolerance);
    m_manager.SetPaused(false);
    Step(4.0f);
    EXPECT_NEAR(m_manager.ParryRush01(), 1.0f, testkit::kTolerance);
    m_manager.ClearOverride();
    Step(0.5f);
    EXPECT_NEAR(m_manager.ParryRush01(), 0.75f, testkit::kTolerance);
}

TEST_F(ParryRushTest, EndingRushPreservesIndependentSlowRequest)
{
    m_manager.SetSlow(0.5f, 0.0f);
    m_manager.BeginParryRush(2.0f, 0.25f, 1.65f);
    Step(0.5f);
    m_manager.EndParryRush();
    Step(0.1f);
    EXPECT_NEAR(Time::timeScale, 0.5f, testkit::kTolerance);
    EXPECT_NEAR(sandbox::TimeManagerComponent::PlayerTimeScale(), 1.0f, testkit::kTolerance);
    EXPECT_NEAR(sandbox::TimeManagerComponent::RushAttackSpeed(), 1.0f, testkit::kTolerance);
}

TEST_F(ParryRushTest, RepeatedSuccessRefreshesWithoutAddingDuration)
{
    m_manager.BeginParryRush(2.0f, 0.25f, 1.65f);
    Step(0.5f);
    m_manager.BeginParryRush(2.0f, 0.25f, 1.65f);
    Step(2.0f);
    EXPECT_FALSE(m_manager.IsParryRush());
    EXPECT_NEAR(Time::timeScale, 1.0f, testkit::kTolerance);
}

TEST_F(ParryRushTest, TutorialExtensionRequiresRealParryAndPreservesAttackSpeed)
{
    m_manager.EnsureParryRushSeconds(4.0f);
    EXPECT_FALSE(m_manager.IsParryRush());
    m_manager.BeginParryRush(1.0f, 0.25f, 1.65f);
    m_manager.EnsureParryRushSeconds(4.0f);
    Step(3.0f);
    EXPECT_TRUE(m_manager.IsParryRush());
    EXPECT_NEAR(sandbox::TimeManagerComponent::RushAttackSpeed(), 1.65f, testkit::kTolerance);
    Step(1.0f);
    EXPECT_FALSE(m_manager.IsParryRush());
}

TEST_F(ParryRushTest, TutorialExtensionDoesNotShortenLongerChance)
{
    m_manager.BeginParryRush(6.0f, 0.25f, 1.65f);
    m_manager.EnsureParryRushSeconds(4.0f);
    Step(5.0f);
    EXPECT_TRUE(m_manager.IsParryRush());
    Step(1.0f);
    EXPECT_FALSE(m_manager.IsParryRush());
}

} // namespace fbzz::tests
