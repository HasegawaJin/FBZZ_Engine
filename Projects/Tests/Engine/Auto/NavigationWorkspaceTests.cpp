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
#include <Engine/Scene/Systems/NavMeshQuery.hpp>
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
        surface.navMesh.SetPolygons({polygon});
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

TEST_F(NavigationWorkspaceTest, IndexedPolygonQueriesPreserveOrderForOverlapsAndNearestTies)
{
    const auto makePolygon = [](float x, float y) {
        scene::NavMeshPolygon polygon;
        polygon.vertices = {{x, y, 0}, {x + 2, y, 0}, {x + 2, y, 2}, {x, y, 2}};
        return polygon;
    };
    scene::NavMesh navMesh;
    navMesh.SetPolygons({makePolygon(4, 10), makePolygon(0, 20), makePolygon(4, 30), makePolygon(20, 40)});
    EXPECT_EQ(scene::FindNearestPolygon(navMesh, {5, 30, 1}), 0);
    EXPECT_FLOAT_EQ(scene::SampleNavMeshHeight(navMesh, {5, 30, 1}), 10);
    EXPECT_EQ(scene::FindNearestPolygon(navMesh, {3, 0, 1}), 0);
    EXPECT_EQ(scene::FindNearestPolygon(navMesh, {21, 0, 1}), 3);
    navMesh.SetPolygons({makePolygon(20, 40), makePolygon(0, 20)});
    EXPECT_EQ(scene::FindNearestPolygon(navMesh, {21, 0, 1}), 0);
}

TEST_F(NavigationWorkspaceTest, IndexedOffMeshLinksRespectDirectionAndAgentMask)
{
    scene::NavMesh navMesh;
    navMesh.SetPolygons({{}, {}, {}});
    scene::OffMeshConnection oneWay;
    oneWay.polyA = 1;
    oneWay.polyB = 2;
    oneWay.bidirectional = false;
    oneWay.agentTypeMask = 1 << 2;
    scene::OffMeshConnection twoWay;
    twoWay.polyA = 1;
    twoWay.polyB = 0;
    navMesh.SetOffMeshLinks({oneWay, twoWay});
    std::vector<int> path;
    EXPECT_TRUE(scene::FindPolygonPath(navMesh, 0, 2, path, -1, nullptr, 2));
    EXPECT_EQ(path, (std::vector<int>{0, 1, 2}));
    EXPECT_FALSE(scene::FindPolygonPath(navMesh, 0, 2, path, -1, nullptr, 0));
    EXPECT_FALSE(scene::FindPolygonPath(navMesh, 2, 0, path, -1, nullptr, 2));
    navMesh.SetOffMeshLinks({});
    EXPECT_FALSE(scene::FindPolygonPath(navMesh, 0, 1, path, -1, nullptr, 2));
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
    auto duplicatePolygon = duplicate.navMesh.Polygons()[0];
    for (auto& point : duplicatePolygon.vertices) point.y = 10;
    duplicate.navMesh.SetPolygons({duplicatePolygon});
    otherType.agentTypeId = 8;
    auto otherTypePolygon = otherType.navMesh.Polygons()[0];
    for (auto& point : otherTypePolygon.vertices) point.y = 20;
    otherType.navMesh.SetPolygons({otherTypePolygon});
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
