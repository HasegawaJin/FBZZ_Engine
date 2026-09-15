/// @file    StageScoringTests.cpp
/// @brief   ステージ別の採点境界と旧記録の更新を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-15
#include <TestKit/TestKit.hpp>
#include <Scripts/UI/StageProgressState.hpp>
#include <array>
#include <limits>

namespace fbzz::tests {
class StageScoringTest : public testkit::Fixture {
protected:
    using Result = sandbox::GameResultState;
    using Progress = sandbox::StageProgressState;
    std::array<sandbox::StageRecord, Progress::kCount> m_records{};
    bool m_victory = false, m_noRetry = true;
    int m_stage = 0, m_chain = 0, m_parries = 0, m_cadences = 0, m_damage = 0;
    float m_seconds = 0;

    void SetUp() override
    {
        Fixture::SetUp();
        std::copy(std::begin(Progress::stages), std::end(Progress::stages), m_records.begin());
        m_victory = Result::victory; m_noRetry = Result::noRetry;
        m_stage = Result::stageIndex; m_chain = Result::bestChain;
        m_parries = Result::parries; m_cadences = Result::perfectCadences;
        m_damage = Result::damageTaken; m_seconds = Result::clearSeconds;
    }
    void TearDown() override
    {
        std::copy(m_records.begin(), m_records.end(), std::begin(Progress::stages));
        Result::victory = m_victory; Result::noRetry = m_noRetry;
        Result::stageIndex = m_stage; Result::bestChain = m_chain;
        Result::parries = m_parries; Result::perfectCadences = m_cadences;
        Result::damageTaken = m_damage; Result::clearSeconds = m_seconds;
        Fixture::TearDown();
    }
};

TEST_F(StageScoringTest, TimeThresholdsAreInclusiveAndStageTwoAllowsTwoBosses)
{
    EXPECT_EQ(Result::TimePoints(300.0f, 0), 2);
    EXPECT_EQ(Result::TimePoints(300.0f, 1), 3);
    for (int stage = 0; stage < 3; ++stage) {
        const auto& p = Result::Profile(stage);
        for (int i = 0; i < 3; ++i) {
            EXPECT_EQ(Result::TimePoints(p.seconds[i], stage), 3 - i);
            EXPECT_EQ(Result::TimePoints(p.seconds[i] + 0.01f, stage), 2 - i);
        }
    }
    EXPECT_EQ(Result::TimePoints(-1.0f, 0), 0);
    EXPECT_EQ(Result::TimePoints(std::numeric_limits<float>::quiet_NaN(), 0), 0);
}

TEST_F(StageScoringTest, OneRepeatedTechniqueCannotEarnAllThreePoints)
{
    EXPECT_EQ(Result::TechniquePoints(1000, 0, 0, 2), 1);
    EXPECT_EQ(Result::TechniquePoints(0, 1000, 0, 2), 1);
    EXPECT_EQ(Result::TechniquePoints(10, 4, 2, 2), 3);
    EXPECT_EQ(Result::TechniquePoints(9, 3, 1, 2), 0);
}

TEST_F(StageScoringTest, LegacyClearMigratesWithoutComparingOldScoreOrTime)
{
    Progress::stages[1] = {};
    auto& record = Progress::stages[1];
    record.cleared = record.unlocked = true;
    record.bestScore = 9; record.bestSeconds = 1.0f;
    EXPECT_FALSE(record.HasCurrentScore());
    Result::stageIndex = 1; Result::victory = true; Result::noRetry = false;
    Result::clearSeconds = 400.0f; Result::bestChain = 8;
    Result::parries = 0; Result::perfectCadences = 0; Result::damageTaken = 1;
    Progress::Commit(1);
    EXPECT_TRUE(record.HasCurrentScore());
    EXPECT_TRUE(record.cleared && record.unlocked);
    EXPECT_EQ(record.bestScore, 5);
    EXPECT_NEAR(record.bestSeconds, 400.0f, 0.001f);
    EXPECT_EQ(record.bestTechnique, 1);
    EXPECT_TRUE(Result::RankAvailable());
    Result::stageIndex = 0;
    Result::clearSeconds = 1.0f;
    Progress::Commit(1);
    EXPECT_NEAR(record.bestSeconds, 400.0f, 0.001f);
}
} // namespace fbzz::tests
