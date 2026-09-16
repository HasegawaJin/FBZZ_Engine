/// @file    BossBreakTests.cpp
/// @brief   崩しの保持・中断・転倒中の入力が次の攻防を壊さないことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-15
#include <TestKit/TestKit.hpp>
#include <TestKit/Deterministic.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Scripts/Combat/BossBreakComponent.hpp>

namespace fbzz::tests {

class BossBreakTest : public testkit::EngineFixture {
protected:
    sandbox::BossBreakComponent m_gauge;
    float m_savedDelta = 0.0f;

    void SetUp() override
    {
        EngineFixture::SetUp();
        m_savedDelta = Time::deltaTime;
        m_gauge.OnStart();
    }

    void TearDown() override
    {
        Time::deltaTime = m_savedDelta;
        EngineFixture::TearDown();
    }

    void Step(int frames, float dt = testkit::kFixedDeltaTime)
    {
        testkit::StepFixed([this](float step) {
            Time::deltaTime = step;
            m_gauge.OnUpdate();
        }, frames, dt);
    }
};

TEST_F(BossBreakTest, ToppledHitsDoNotBankHeatOrParryStreak)
{
    m_gauge.AddSlash(false);
    m_gauge.BeginTopple(9.0f);

    m_gauge.AddSlash(true);
    m_gauge.AddParry(true, true);

    EXPECT_EQ(m_gauge.Edge(), 1);
    EXPECT_EQ(m_gauge.ParryStreak(), 0);
    EXPECT_TRUE(m_gauge.IsToppled());
}

TEST_F(BossBreakTest, DisabledGainDoesNotRefreshDecayOrBankBonuses)
{
    m_gauge.Add(40.0f, "Setup");
    m_gauge.SetGainScale(0.0f);
    Step(1, 3.0f);

    m_gauge.AddSlash(false);
    m_gauge.AddParry(false);
    m_gauge.AddPerfectDodge();

    EXPECT_NEAR(m_gauge.Ratio(), 0.36f, 0.0001f);
    EXPECT_NEAR(m_gauge.SinceLastGain(), 3.0f, 0.0001f);
    EXPECT_EQ(m_gauge.Edge(), 0);
    EXPECT_EQ(m_gauge.ParryStreak(), 0);
}

TEST_F(BossBreakTest, DecayOnlyCountsTimeAfterTheHoldBoundary)
{
    m_gauge.Add(40.0f, "Setup");

    Step(1, 2.4f);
    Step(1, 0.2f);

    EXPECT_NEAR(m_gauge.Ratio(), 0.392f, 0.0001f);
}

TEST_F(BossBreakTest, ForcedWaitPreservesGaugeAndRemainingHold)
{
    m_gauge.Add(40.0f, "Setup");
    Step(1, 2.0f);
    m_gauge.SetDecayPaused(true);

    Step(600);
    EXPECT_NEAR(m_gauge.Ratio(), 0.4f, 0.0001f);
    m_gauge.SetDecayPaused(false);
    Step(1, 0.5f);
    EXPECT_NEAR(m_gauge.Ratio(), 0.4f, 0.0001f);
    Step(1, 0.5f);
    EXPECT_NEAR(m_gauge.Ratio(), 0.36f, 0.0001f);
}

TEST_F(BossBreakTest, FullGaugeEntersToppleOnceAndRestartsAfterExecution)
{
    int breaks = 0;
    m_gauge.onBreak = [this, &breaks](float seconds) {
        ++breaks;
        m_gauge.BeginTopple(seconds);
    };

    m_gauge.Add(100.0f, "First");
    m_gauge.AddParry(true);
    m_gauge.Add(100.0f, "Duplicate");
    EXPECT_EQ(breaks, 1);
    m_gauge.EndTopple();
    EXPECT_NEAR(m_gauge.Ratio(), 0.0f, 0.0001f);
    m_gauge.Add(100.0f, "Second");
    EXPECT_EQ(breaks, 2);
}

} // namespace fbzz::tests
