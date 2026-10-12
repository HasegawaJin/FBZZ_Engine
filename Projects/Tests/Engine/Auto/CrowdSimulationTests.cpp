/// @file    CrowdSimulationTests.cpp
/// @brief   Crowd budget, perception, threat latency, replay and continuous movement contracts.
/// @author  Hasegawa Jin
/// @date    2026-10-11
#include <TestKit/Fixture.hpp>
#include <Engine/AI/CrowdSimulation.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace fbzz::tests {
class CrowdSimulationTest : public testkit::Fixture {
protected:
    ai::CrowdSettings settings;
    std::vector<ai::CrowdProfile> profiles{ai::CrowdProfile{}};
    std::vector<ai::CrowdAgent> input, output;
    ai::CrowdWorkspace workspace;
    ai::CrowdStats stats;
    void Populate(std::size_t count) {
        settings.minimum = {-100, -100, -100}; settings.maximum = {100, 100, 100}; settings.deltaSeconds = 1.0f / 60;
        input.resize(count); output.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            auto& a = input[i]; a.seed = static_cast<std::uint32_t>(i + 1);
            a.floor = -100; a.ceiling = 100; a.goal = {10, 0, 10};
            a.position = {static_cast<float>(i % 10), static_cast<float>((i / 10) % 10), static_cast<float>(i / 100)};
        }
    }
    bool Step(std::span<const ai::CrowdThreat> threats = {}, std::span<const ai::CrowdObstacle> obstacles = {}) {
        return ai::StepCrowd(settings, profiles, input, threats, obstacles, output, stats, workspace);
    }
    void Advance() { input.swap(output); settings.decisionCursor = stats.decisionCursor; }
};

TEST_F(CrowdSimulationTest, ThousandAgentsRespectCandidateAndDecisionBudgets) {
    Populate(1000); settings.maximumDecisions = 128;
    ASSERT_TRUE(Step());
    EXPECT_EQ(stats.decisions, 128U); EXPECT_EQ(stats.deferredDecisions, 872U);
    EXPECT_LE(stats.candidateTests, stats.decisions * settings.candidateLimit);
    EXPECT_LE(stats.neighbors, stats.decisions * settings.neighborLimit);
    EXPECT_GT(stats.occupiedCells, 0U);
}
TEST_F(CrowdSimulationTest, LongFrameRetainsDecisionAndThreatClockWhileClampingMovement) {
    Populate(1); settings.deltaSeconds = 0.5f; settings.maximumDelta = 0.1f; settings.maximumDecisions = 0;
    input[0].decisionIn = 1; input[0].decisionAge = 2; input[0].threatIn = 1; input[0].fear = 1;
    ASSERT_TRUE(Step());
    EXPECT_FLOAT_EQ(output[0].decisionIn, 0.5f);
    EXPECT_FLOAT_EQ(output[0].decisionAge, 2.5f);
    EXPECT_FLOAT_EQ(output[0].threatIn, 0.5f);
    EXPECT_FLOAT_EQ(output[0].fear, 1 - settings.fearDecay * 0.5f);
    EXPECT_LE(output[0].velocity.Length(), profiles[0].maximumAcceleration * settings.maximumDelta + 0.00001f);
}
TEST_F(CrowdSimulationTest, RoundRobinBudgetDoesNotStarveDueAgents) {
    Populate(1000); settings.maximumDecisions = 100; settings.nearInterval = settings.midInterval = settings.farInterval = 0;
    std::vector<bool> seen(1000);
    for (int step = 0; step < 10; ++step) {
        ASSERT_TRUE(Step());
        for (std::size_t i = 0; i < input.size(); ++i) if (output[i].decisionAge == 0) seen[i] = true;
        Advance();
    }
    EXPECT_TRUE(std::all_of(seen.begin(), seen.end(), [](bool value) { return value; }));
}
TEST_F(CrowdSimulationTest, WanderChoosesGoalsInsideEachCachedVerticalHabitat) {
    Populate(256);
    for (auto& agent : input) { agent.floor = 2; agent.ceiling = 2.5f; agent.position.y = 2.2f; agent.goal = agent.position; }
    ASSERT_TRUE(Step());
    for (const auto& agent : output) {
        EXPECT_GE(agent.goal.y, agent.floor + profiles[0].bodyRadius);
        EXPECT_LE(agent.goal.y, agent.ceiling - profiles[0].bodyRadius);
    }
}
TEST_F(CrowdSimulationTest, ThreatBypassesFarLodAndZeroDecisionBudgetWithinOneUpdate) {
    Populate(1); input[0].decisionIn = 10; input[0].desiredVelocity = {0, 0, 1}; settings.maximumDecisions = 0;
    ai::CrowdThreat threat; threat.position = {1, 0, 0};
    ASSERT_TRUE(Step({&threat, 1}));
    EXPECT_EQ(stats.decisions, 0U); EXPECT_EQ(stats.threatReactions, 1U);
    EXPECT_LT(output[0].velocity.x, 0); EXPECT_EQ(output[0].mode, ai::CrowdMode::FLEE);
    EXPECT_NE(output[0].events & ai::CrowdEvents::THREAT_ENTERED, 0U);
    EXPECT_GT(output[0].decisionIn, 9);
}
TEST_F(CrowdSimulationTest, ThreatExitIsOneShotAndDoesNotLoseElapsedTime) {
    Populate(1); input[0].threatIn = 0.02f; input[0].decisionIn = 5; settings.maximumDecisions = 0;
    ASSERT_TRUE(Step()); Advance(); ASSERT_TRUE(Step());
    EXPECT_EQ(output[0].threatIn, 0); EXPECT_NE(output[0].events & ai::CrowdEvents::THREAT_LEFT, 0U);
    EXPECT_NEAR(output[0].decisionAge, 2.0f / 60, 1e-6f);
    Advance(); ASSERT_TRUE(Step()); EXPECT_EQ(output[0].events & ai::CrowdEvents::THREAT_LEFT, 0U);
}
TEST_F(CrowdSimulationTest, NearMidFarIntervalsOnlyReduceDecisions) {
    Populate(3); input[0].position = {}; input[1].position = {40, 0, 0}; input[2].position = {90, 0, 0};
    ASSERT_TRUE(Step());
    EXPECT_FLOAT_EQ(output[0].decisionIn, settings.nearInterval);
    EXPECT_FLOAT_EQ(output[1].decisionIn, settings.midInterval);
    EXPECT_FLOAT_EQ(output[2].decisionIn, settings.farInterval);
    Advance(); ASSERT_TRUE(Step()); EXPECT_EQ(stats.decisions, 0U);
    for (const auto& a : output) EXPECT_GT(a.velocity.LengthSq(), 0);
}
TEST_F(CrowdSimulationTest, BoundedPerceptionCanSenseAnotherGroupWithoutFlockingWithIt) {
    Populate(3); input[0].senseGroup = 2; input[1].group = 1; input[2].group = 2;
    input[2].position = {0.5f, 0, 0};
    ASSERT_TRUE(Step()); EXPECT_EQ(output[0].sensedIndex, 2); EXPECT_NEAR(output[0].sensedDistance, 0.5f, 1e-6f);
}
TEST_F(CrowdSimulationTest, ExternalMovementRespectsAccelerationAndTurnRate) {
    Populate(1); input[0].intent = ai::CrowdIntent::EXTERNAL;
    input[0].velocity = {0, 0, 1}; input[0].externalVelocity = {-1, 0, 0};
    ASSERT_TRUE(Step());
    EXPECT_LE((output[0].velocity - input[0].velocity).Length(), profiles[0].maximumAcceleration * settings.deltaSeconds + 1e-5f);
    EXPECT_LE(stats.maximumTurn, profiles[0].maximumTurnRate * settings.deltaSeconds + 0.001f);
}
TEST_F(CrowdSimulationTest, OppositeDirectionTurnHasDeterministicTangent) {
    Populate(1); input[0].intent = ai::CrowdIntent::EXTERNAL; input[0].velocity = {0, 0, 1}; input[0].externalVelocity = {0, 0, -1};
    ASSERT_TRUE(Step()); EXPECT_GT(output[0].velocity.z, 0); EXPECT_GT(std::abs(output[0].velocity.x), 0);
}
TEST_F(CrowdSimulationTest, SafetyUsesBodyRadiusAndCachedFloorCeilingEveryUpdate) {
    Populate(1); input[0].floor = -0.5f; input[0].ceiling = 0.5f; input[0].position = {0, 0.49f, 0}; input[0].decisionIn = 5;
    ASSERT_TRUE(Step()); EXPECT_LE(output[0].position.y, 0.5f - profiles[0].bodyRadius);
    EXPECT_NE(output[0].events & ai::CrowdEvents::CONSTRAINED, 0U);
}
TEST_F(CrowdSimulationTest, CollapsedVerticalEnvelopeStopsInsteadOfProducingNan) {
    Populate(1); input[0].floor = 0; input[0].ceiling = 0.1f;
    ASSERT_TRUE(Step()); EXPECT_FLOAT_EQ(output[0].velocity.LengthSq(), 0);
    EXPECT_NE(output[0].events & ai::CrowdEvents::CONSTRAINED, 0U);
}
TEST_F(CrowdSimulationTest, ObstaclePenetrationProjectsOutsideBodyClearance) {
    Populate(1); ai::CrowdObstacle obstacle; obstacle.radius = 0.5f;
    ASSERT_TRUE(Step({}, {&obstacle, 1}));
    EXPECT_GE((output[0].position - obstacle.position).Length(), 0.5f + profiles[0].bodyRadius - 1e-5f);
}
TEST_F(CrowdSimulationTest, ArrivalDoesNotInstantlyRemoveVelocityBeyondAccelerationBudget) {
    Populate(1); input[0].intent = ai::CrowdIntent::ARRIVE; input[0].goal = {0, 0, 0.21f}; input[0].velocity = {0, 0, 1};
    ASSERT_TRUE(Step()); EXPECT_LE((output[0].velocity - input[0].velocity).Length(), profiles[0].maximumAcceleration * settings.deltaSeconds + 1e-5f);
    EXPECT_NE(output[0].events & ai::CrowdEvents::ARRIVED, 0U);
}
TEST_F(CrowdSimulationTest, IdenticalSeedSnapshotsReplayExactlyWithoutInputMutation) {
    Populate(1000); auto replay = input; auto first = output; ai::CrowdWorkspace other; ai::CrowdStats otherStats;
    for (int step = 0; step < 60; ++step) {
        ASSERT_TRUE(Step()); ASSERT_TRUE(ai::StepCrowd(settings, profiles, replay, {}, {}, first, otherStats, other));
        for (std::size_t i = 0; i < input.size(); ++i) {
            EXPECT_FLOAT_EQ(output[i].position.x, first[i].position.x); EXPECT_FLOAT_EQ(output[i].position.y, first[i].position.y);
            EXPECT_FLOAT_EQ(output[i].position.z, first[i].position.z); EXPECT_EQ(output[i].seed, first[i].seed);
        }
        replay.swap(first); Advance();
    }
}
TEST_F(CrowdSimulationTest, InvalidOrAliasedInputsLeaveOutputsAndStatsUnchanged) {
    Populate(1); output[0].fear = 0.7f; stats.decisions = 123;
    input[0].velocity.x = std::numeric_limits<float>::quiet_NaN(); EXPECT_FALSE(Step());
    EXPECT_FLOAT_EQ(output[0].fear, 0.7f); EXPECT_EQ(stats.decisions, 123U);
    input[0].velocity = {}; EXPECT_FALSE(ai::StepCrowd(settings, profiles, input, {}, {}, input, stats, workspace));
}
TEST_F(CrowdSimulationTest, InactiveSlotsRetainStateAndEmitNoRepeatedEvents) {
    Populate(1); input[0].flags = 0; input[0].events = 63; input[0].position = {1, 2, 3};
    ASSERT_TRUE(Step()); EXPECT_EQ(output[0].events, 0U); EXPECT_FLOAT_EQ(output[0].position.x, 1);
    EXPECT_EQ(stats.decisions, 0U);
}
}
