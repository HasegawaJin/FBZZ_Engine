/// @file    BehaviorTreeSystemTests.cpp
/// @brief   Scene-level BT clock wake and patrol ownership across throttling, pause and disable.
/// @author  Hasegawa Jin
/// @date    2026-10-11
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/AI/BehaviorTreeEvaluator.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Physics/World.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/BehaviorTreeSystem.hpp>
#include <Engine/Scene/Systems/NavMeshPatrolSystem.hpp>
#include <memory>

namespace fbzz::tests {
class BehaviorTreeSystemTest : public testkit::EngineFixture {
protected:
    scene::Scene scene;
    physics::World world;
    scene::BehaviorTreeSystem behavior;
    scene::NavMeshPatrolSystem patrol;
    SystemContext Context(float dt) { return {scene, world, nullptr, nullptr, dt, 0, true, true}; }
    bool Attach(scene::GameObject& object, ai::BTNodeType type, float duration = 1) {
        ai::BTNodeDef node; node.id = 1; node.type = type; node.duration = duration;
        ai::BehaviorTreeAsset asset; asset.nodes.push_back(node);
        auto runtime = std::make_shared<ai::BehaviorTreeRuntime>();
        if (!ai::CompileBehaviorTree(asset, *runtime)) return false;
        auto& bt = object.AddComponent<scene::BehaviorTreeComponent>();
        bt.runtime = runtime; bt.initialized = true; bt.loadedTreePath = bt.treePath;
        bt.blackboard.Reset(runtime->blackboard); bt.state.Resize(runtime->NodeCount());
        bt.evaluatedRevision = bt.blackboard.GetRevision(); bt.lastNodeStatus.resize(runtime->NodeCount());
        bt.tickRate = 0.5f; bt.tickTimer = 0;
        return true;
    }
    void Route(scene::GameObject& object) {
        object.AddComponent<scene::NavMeshAgentComponent>();
        auto& route = object.AddComponent<scene::NavMeshPatrolComponent>();
        route.waypoints = {{10, 0, 0}, {20, 0, 0}};
    }
};

TEST_F(BehaviorTreeSystemTest, LoadedWaitingTreeSuppressesAutonomousPatrolWithoutChangingAuthoredEnabled) {
    auto& object = scene.CreateGameObject("Agent"); Route(object);
    auto ctx = Context(0.1f); patrol.Update(ctx);
    EXPECT_TRUE(object.GetComponent<scene::NavMeshAgentComponent>()->hasDestination);
    object.GetComponent<scene::NavMeshAgentComponent>()->Stop();
    ASSERT_TRUE(Attach(object, ai::BTNodeType::Wait));
    behavior.Update(ctx); patrol.Update(ctx);
    EXPECT_FALSE(object.GetComponent<scene::NavMeshAgentComponent>()->hasDestination);
    EXPECT_TRUE(object.GetComponent<scene::NavMeshPatrolComponent>()->enabled);
}

TEST_F(BehaviorTreeSystemTest, PatrolPermissionPersistsBetweenTicksAndPauseDisableStopOwnedMotion) {
    auto& object = scene.CreateGameObject("Agent"); Route(object);
    ASSERT_TRUE(Attach(object, ai::BTNodeType::Patrol));
    auto ctx = Context(0.1f); behavior.Update(ctx); patrol.Update(ctx);
    auto* bt = object.GetComponent<scene::BehaviorTreeComponent>();
    auto* agent = object.GetComponent<scene::NavMeshAgentComponent>();
    EXPECT_TRUE(bt->patrolRequested); EXPECT_TRUE(agent->hasDestination); EXPECT_EQ(bt->tickCount, 1U);
    behavior.Update(ctx); patrol.Update(ctx);
    EXPECT_TRUE(bt->patrolRequested); EXPECT_TRUE(agent->hasDestination); EXPECT_EQ(bt->tickCount, 1U);
    bt->startPaused = true; behavior.Update(ctx); patrol.Update(ctx);
    EXPECT_FALSE(bt->patrolRequested); EXPECT_FALSE(agent->hasDestination);
    bt->startPaused = false; bt->RequestEvaluation(); behavior.Update(ctx); patrol.Update(ctx);
    EXPECT_TRUE(bt->patrolRequested); EXPECT_TRUE(agent->hasDestination);
    bt->enabled = false; behavior.Update(ctx); patrol.Update(ctx);
    EXPECT_FALSE(bt->patrolRequested); EXPECT_FALSE(agent->hasDestination);
    EXPECT_TRUE(object.GetComponent<scene::NavMeshPatrolComponent>()->enabled);
}

TEST_F(BehaviorTreeSystemTest, ChangedBlackboardWakesEarlyWithActualElapsedWaitTime) {
    auto& object = scene.CreateGameObject("Agent");
    ASSERT_TRUE(Attach(object, ai::BTNodeType::Wait, 0.35f));
    auto ctx = Context(0.1f); behavior.Update(ctx);
    auto* bt = object.GetComponent<scene::BehaviorTreeComponent>();
    EXPECT_EQ(bt->lastRootStatus, ai::BTStatus::Running);
    behavior.Update(ctx); EXPECT_EQ(bt->tickCount, 1U);
    ASSERT_TRUE(bt->blackboard.SetFloat(ai::bb::Health01, 0.5f));
    behavior.Update(ctx); EXPECT_EQ(bt->tickCount, 2U); EXPECT_EQ(bt->lastRootStatus, ai::BTStatus::Running);
    ASSERT_TRUE(bt->blackboard.SetFloat(ai::bb::Health01, 0.4f));
    behavior.Update(ctx); EXPECT_EQ(bt->tickCount, 3U); EXPECT_EQ(bt->lastRootStatus, ai::BTStatus::Success);
    EXPECT_FLOAT_EQ(bt->elapsedTime, 0.4f);
}
}
