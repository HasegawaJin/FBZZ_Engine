/// @file    NavigationWorkspaceTests.cpp
/// @brief   Navigation 作業領域の再利用時の回避・Scene 切替・通知中の追加を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-09
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/NavMeshAgentComponent.hpp>
#include <Engine/Scene/Components/NavMeshSurfaceComponent.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/Systems/NavigationSystem.hpp>
#include <Physics/World.hpp>
#include <memory>

namespace fbzz::tests {
namespace {
class NavigationWorkspaceTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    physics::World m_world;
    scene::NavigationSystem m_system;

    scene::NavMeshSurfaceComponent& Surface(scene::Scene& scene)
    {
        auto& surface = scene.CreateGameObject("Surface").AddComponent<scene::NavMeshSurfaceComponent>();
        scene::NavMeshPolygon polygon;
        polygon.vertices = {{-100, 0, -100}, {100, 0, -100}, {100, 0, 100}, {-100, 0, 100}};
        surface.navMesh.polygons.push_back(polygon);
        return surface;
    }

    scene::GameObject& Agent(scene::Scene& scene, const math::Vector3& position)
    {
        auto& object = scene.CreateGameObject("Agent");
        object.transform.position = position;
        auto& agent = object.AddComponent<scene::NavMeshAgentComponent>();
        agent.SetDestination(position + math::Vector3{0, 0, 10});
        agent.path = {agent.destination};
        agent.maxSpeed = 1;
        agent.acceleration = 10;
        agent.autoBraking = false;
        agent.updateRotation = false;
        return object;
    }

    void Update(scene::Scene& scene)
    {
        SystemContext context{scene, m_world, nullptr, nullptr, 0.1f, 0.1f, true, true};
        m_system.Update(context);
    }
};

class AppendAgentOnArrival final : public scene::Script {
public:
    AppendAgentOnArrival(scene::Scene& scene, scene::EntityID& created) : m_scene(scene), m_created(created) {}
    void OnNavMeshDestinationReached() override
    {
        for (int i = 0; i < 128; ++i) {
            auto& object = m_scene.CreateGameObject("Added by arrival");
            auto& agent = object.AddComponent<scene::NavMeshAgentComponent>();
            agent.SetDestination({0, 0, 10});
            agent.path = {agent.destination};
            agent.maxSpeed = 1;
            agent.acceleration = 10;
            agent.updateRotation = false;
            agent.autoBraking = false;
            if (i == 0) m_created = object.GetID();
        }
    }
private:
    scene::Scene& m_scene;
    scene::EntityID& m_created;
};

TEST_F(NavigationWorkspaceTest, RebuildsOccupiedBucketsAfterAgentsMoveBetweenFrames)
{
    Surface(m_scene);
    auto& first = Agent(m_scene, {0, 0, 0});
    auto& second = Agent(m_scene, {0.2f, 0, 0});
    Update(m_scene);
    EXPECT_LT(first.GetComponent<scene::NavMeshAgentComponent>()->velocity.x, 0);
    EXPECT_GT(second.GetComponent<scene::NavMeshAgentComponent>()->velocity.x, 0);
    first.transform.position = {-20, 0, 0};
    second.transform.position = {20, 0, 0};
    auto& firstAgent = *first.GetComponent<scene::NavMeshAgentComponent>();
    auto& secondAgent = *second.GetComponent<scene::NavMeshAgentComponent>();
    firstAgent.SetDestination({-20, 0, 10}); firstAgent.path = {firstAgent.destination};
    secondAgent.SetDestination({20, 0, 10}); secondAgent.path = {secondAgent.destination};
    Update(m_scene);
    EXPECT_NEAR(firstAgent.velocity.x, 0, testkit::kTolerance);
    EXPECT_NEAR(secondAgent.velocity.x, 0, testkit::kTolerance);
    EXPECT_NEAR(firstAgent.velocity.z, 1, testkit::kTolerance);
    EXPECT_NEAR(secondAgent.velocity.z, 1, testkit::kTolerance);
}

TEST_F(NavigationWorkspaceTest, ReusesOneSystemAcrossDifferentScenesWithoutRetainingSurfaceOrAgentReferences)
{
    Surface(m_scene);
    Agent(m_scene, {});
    Update(m_scene);
    m_scene.Clear();
    Update(m_scene);
    scene::Scene other;
    Surface(other);
    auto& object = Agent(other, {3, 0, -2});
    Update(other);
    EXPECT_VEC3_NEAR(object.transform.worldPosition, (math::Vector3{3, 0, -1.9f}), testkit::kTolerance);
    other.Clear();
    Surface(m_scene);
    auto& replacement = Agent(m_scene, {-3, 0, -2});
    Update(m_scene);
    EXPECT_VEC3_NEAR(replacement.transform.worldPosition, (math::Vector3{-3, 0, -1.9f}), testkit::kTolerance);
}

TEST_F(NavigationWorkspaceTest, PreservesTheFirstValidSurfaceForEachTypeAndRechecksDisabledSurfaces)
{
    auto& first = Surface(m_scene);
    auto& duplicate = Surface(m_scene);
    auto& otherType = Surface(m_scene);
    for (auto& point : duplicate.navMesh.polygons[0].vertices) point.y = 10;
    otherType.agentTypeId = 8;
    for (auto& point : otherType.navMesh.polygons[0].vertices) point.y = 20;
    auto& firstObject = Agent(m_scene, {});
    auto& secondObject = Agent(m_scene, {5, 0, 0});
    auto& firstAgent = *firstObject.GetComponent<scene::NavMeshAgentComponent>();
    auto& secondAgent = *secondObject.GetComponent<scene::NavMeshAgentComponent>();
    firstAgent.Stop(); firstAgent.snapToNavMesh = true;
    secondAgent.Stop(); secondAgent.snapToNavMesh = true; secondAgent.agentTypeId = 8;
    Update(m_scene);
    EXPECT_FLOAT_EQ(firstObject.transform.worldPosition.y, 0);
    EXPECT_FLOAT_EQ(secondObject.transform.worldPosition.y, 20);
    first.enabled = false;
    Update(m_scene);
    EXPECT_FLOAT_EQ(firstObject.transform.worldPosition.y, 10);
    EXPECT_FLOAT_EQ(secondObject.transform.worldPosition.y, 20);
}

TEST_F(NavigationWorkspaceTest, DefersAgentsAddedByAnArrivalNotificationUntilTheNextUpdate)
{
    Surface(m_scene);
    auto& arriving = Agent(m_scene, {});
    auto& agent = *arriving.GetComponent<scene::NavMeshAgentComponent>();
    agent.SetDestination({}); agent.path = {{}};
    scene::EntityID created = scene::EntityID::INVALID;
    arriving.AddComponent<scene::ScriptComponent>().scripts.emplace_back().script =
        std::make_unique<AppendAgentOnArrival>(m_scene, created);
    Update(m_scene);
    ASSERT_TRUE(m_scene.IsValid(created));
    auto* added = m_scene.GetGameObject(created);
    ASSERT_NE(added, nullptr);
    EXPECT_VEC3_NEAR(added->transform.position, math::Vector3::ZERO, testkit::kTolerance);
    EXPECT_VEC3_NEAR(added->GetComponent<scene::NavMeshAgentComponent>()->velocity, math::Vector3::ZERO, testkit::kTolerance);
    Update(m_scene);
    EXPECT_GT(added->GetComponent<scene::NavMeshAgentComponent>()->velocity.z, 0);
}
} /// @note namespace
} /// @note namespace fbzz::tests
