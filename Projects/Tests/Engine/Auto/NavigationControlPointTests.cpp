/// @file    NavigationControlPointTests.cpp
/// @brief   Verify navigation point resolution, stable attributes, and static bake behavior.
/// @author  Hasegawa Jin
/// @date    2026-10-08
#include <TestKit/TestKit.hpp>

#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Scene/Components/NavMeshAgentComponent.hpp>
#include <Engine/Scene/Components/NavMeshPatrolComponent.hpp>
#include <Engine/Scene/Components/NavMeshSurfaceComponent.hpp>
#include <Engine/Scene/Components/TargetPointComponent.hpp>
#include <Engine/Scene/NavigationControlPoints.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/NavMeshPatrolSystem.hpp>
#include <Engine/Scene/Systems/NavigationSystem.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Math/Vector3.hpp>
#include <Physics/World.hpp>
#include <algorithm>
#include <limits>

namespace fbzz::tests {
namespace {

class NavigationControlPointTest : public testkit::Fixture {
protected:
    scene::Scene m_scene;
};

TEST_F(NavigationControlPointTest, InlineRoutePreservesLegacyWorldWaypointsAndFallbackAttributes)
{
    auto& owner = m_scene.CreateGameObject("Patrol");
    auto& patrol = owner.AddComponent<scene::NavMeshPatrolComponent>();
    patrol.waypoints = {{3.0f, 1.0f, -2.0f}, {8.0f, 0.0f, 4.0f}};
    patrol.waypointWaitTimes = {0.25f};
    patrol.waypointSpeeds = {2.5f, 0.0f};
    patrol.waitTime = 0.75f;

    const auto snapshot = scene::ResolveNavMeshPatrolPointAuthoring(
        m_scene, owner.GetID(), patrol, scene::ControlPointRuntimeMode::LIVE);
    ASSERT_TRUE(snapshot.valid);
    ASSERT_EQ(snapshot.points.size(), 2u);
    EXPECT_EQ(snapshot.points[0].stableId, 1u);
    EXPECT_VEC3_NEAR(snapshot.points[0].worldPosition, (math::Vector3{3.0f, 1.0f, -2.0f}), testkit::kTolerance);
    EXPECT_FLOAT_EQ(snapshot.points[0].waitTime, 0.25f);
    EXPECT_FLOAT_EQ(snapshot.points[0].speed, 2.5f);
    EXPECT_EQ(snapshot.points[1].stableId, 2u);
    EXPECT_VEC3_NEAR(snapshot.points[1].worldPosition, (math::Vector3{8.0f, 0.0f, 4.0f}), testkit::kTolerance);
    EXPECT_FLOAT_EQ(snapshot.points[1].waitTime, 0.75f);
    EXPECT_FLOAT_EQ(snapshot.points[1].speed, 0.0f);
}

TEST_F(NavigationControlPointTest, PointBindingsKeepIdentityAndAttributesWithTheirRows)
{
    auto& owner = m_scene.CreateGameObject("Patrol");
    auto& first = m_scene.CreateGameObject("First");
    auto& second = m_scene.CreateGameObject("Second");
    first.transform.position = {2.0f, 0.0f, 1.0f};
    second.transform.position = {9.0f, 3.0f, -4.0f};
    first.AddComponent<scene::TargetPointComponent>();
    second.AddComponent<scene::TargetPointComponent>();
    auto& patrol = owner.AddComponent<scene::NavMeshPatrolComponent>();
    patrol.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    scene::NavMeshPatrolWaypointBinding firstBinding;
    firstBinding.id = 71;
    firstBinding.target = {first.instanceId, first.GetID(), m_scene.GetRenderSceneGeneration()};
    firstBinding.waitTime = 0.2f;
    firstBinding.speed = 1.5f;
    scene::NavMeshPatrolWaypointBinding secondBinding;
    secondBinding.id = 93;
    secondBinding.target = {second.instanceId, second.GetID(), m_scene.GetRenderSceneGeneration()};
    secondBinding.waitTime = 0.8f;
    secondBinding.speed = 4.0f;
    patrol.waypointBindings = {firstBinding, secondBinding};
    scene::FlushWorldTransforms(m_scene);

    auto snapshot = scene::ResolveNavMeshPatrolPointAuthoring(
        m_scene, owner.GetID(), patrol, scene::ControlPointRuntimeMode::LIVE);
    ASSERT_TRUE(snapshot.valid);
    ASSERT_EQ(snapshot.points.size(), 2u);
    EXPECT_EQ(snapshot.points[0].stableId, 71u);
    EXPECT_FLOAT_EQ(snapshot.points[0].waitTime, 0.2f);
    EXPECT_FLOAT_EQ(snapshot.points[0].speed, 1.5f);
    EXPECT_VEC3_NEAR(snapshot.points[0].worldPosition, first.transform.worldPosition, testkit::kTolerance);

    std::reverse(patrol.waypointBindings.begin(), patrol.waypointBindings.end());
    snapshot = scene::ResolveNavMeshPatrolPointAuthoring(
        m_scene, owner.GetID(), patrol, scene::ControlPointRuntimeMode::LIVE);
    ASSERT_TRUE(snapshot.valid);
    ASSERT_EQ(snapshot.points.size(), 2u);
    EXPECT_EQ(snapshot.points[0].stableId, 93u);
    EXPECT_FLOAT_EQ(snapshot.points[0].waitTime, 0.8f);
    EXPECT_FLOAT_EQ(snapshot.points[0].speed, 4.0f);
    EXPECT_EQ(snapshot.points[1].stableId, 71u);
    EXPECT_FLOAT_EQ(snapshot.points[1].waitTime, 0.2f);
    EXPECT_FLOAT_EQ(snapshot.points[1].speed, 1.5f);
}

TEST_F(NavigationControlPointTest, MissingPointInvalidatesTheWholeRouteAndKeepsAChangedSignature)
{
    auto& owner = m_scene.CreateGameObject("Patrol");
    auto& target = m_scene.CreateGameObject("Present");
    target.AddComponent<scene::TargetPointComponent>();
    auto& patrol = owner.AddComponent<scene::NavMeshPatrolComponent>();
    patrol.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    scene::NavMeshPatrolWaypointBinding validBinding;
    validBinding.id = 10;
    validBinding.target = {target.instanceId, target.GetID(), m_scene.GetRenderSceneGeneration()};
    scene::NavMeshPatrolWaypointBinding missingBinding;
    missingBinding.id = 11;
    missingBinding.target.guid = "missing-waypoint";
    patrol.waypointBindings = {validBinding, missingBinding};
    scene::FlushWorldTransforms(m_scene);

    const auto snapshot = scene::ResolveNavMeshPatrolPointAuthoring(
        m_scene, owner.GetID(), patrol, scene::ControlPointRuntimeMode::LIVE);
    EXPECT_FALSE(snapshot.valid);
    EXPECT_TRUE(snapshot.points.empty());
    EXPECT_EQ(snapshot.status, scene::NavigationControlPointStatus::RESOLUTION_FAILED);
    EXPECT_EQ(snapshot.resolveStatus, scene::ControlPointResolveStatus::UNRESOLVED_REFERENCE);
    EXPECT_NE(snapshot.inputSignature, 0u);
}

TEST_F(NavigationControlPointTest, StaticRuntimeUsesCapturedNumbersWithoutResolvingTargets)
{
    auto& owner = m_scene.CreateGameObject("Patrol");
    auto& target = m_scene.CreateGameObject("Waypoint");
    target.transform.position = {5.0f, 1.0f, 7.0f};
    target.AddComponent<scene::TargetPointComponent>();
    auto& patrol = owner.AddComponent<scene::NavMeshPatrolComponent>();
    patrol.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    scene::NavMeshPatrolWaypointBinding binding;
    binding.id = 45;
    binding.target = {target.instanceId, target.GetID(), m_scene.GetRenderSceneGeneration()};
    binding.waitTime = 1.25f;
    binding.speed = 3.0f;
    patrol.waypointBindings.push_back(binding);
    scene::FlushWorldTransforms(m_scene);

    const auto authoring = scene::ResolveNavMeshPatrolPointAuthoring(
        m_scene, owner.GetID(), patrol, scene::ControlPointRuntimeMode::BAKED_STATIC);
    ASSERT_TRUE(authoring.valid);
    patrol.bakedWaypoints.push_back({authoring.points[0].stableId,
        authoring.points[0].worldPosition, authoring.points[0].waitTime, authoring.points[0].speed});
    patrol.bakedInputSignature = authoring.inputSignature;
    patrol.bakedSourceMode = patrol.pointSourceMode;
    patrol.bakedAdapterVersion = scene::NAV_MESH_PATROL_ADAPTER_VERSION;
    patrol.bakeValid = true;
    patrol.pointRuntimeMode = scene::ControlPointRuntimeMode::BAKED_STATIC;
    const math::Vector3 capturedPosition = patrol.bakedWaypoints[0].worldPosition;

    target.transform.position = {50.0f, 10.0f, -20.0f};
    scene::FlushWorldTransforms(m_scene);
    patrol.waypointBindings[0].target.guid.clear();
    patrol.waypointBindings[0].target.id = target.GetID();
    const auto runtime = scene::ResolveNavMeshPatrolPoints(
        m_scene, owner.GetID(), patrol, scene::ControlPointReadContext::RUNTIME);
    ASSERT_TRUE(runtime.valid);
    ASSERT_EQ(runtime.points.size(), 1u);
    EXPECT_VEC3_NEAR(runtime.points[0].worldPosition, capturedPosition, testkit::kTolerance);
    EXPECT_FLOAT_EQ(runtime.points[0].waitTime, 1.25f);
    EXPECT_FLOAT_EQ(runtime.points[0].speed, 3.0f);

    const auto authoringCheck = scene::ResolveNavMeshPatrolPoints(
        m_scene, owner.GetID(), patrol, scene::ControlPointReadContext::AUTHORING);
    EXPECT_TRUE(authoringCheck.bakeStale);
    EXPECT_FALSE(authoringCheck.valid);
    EXPECT_TRUE(authoringCheck.points.empty());
    EXPECT_EQ(authoringCheck.status, scene::NavigationControlPointStatus::RESOLUTION_FAILED);
    EXPECT_EQ(authoringCheck.resolveStatus, scene::ControlPointResolveStatus::NULL_REFERENCE);
    EXPECT_NE(authoringCheck.inputSignature, 0u);
    EXPECT_NE(authoringCheck.inputSignature, patrol.bakedInputSignature);
}

TEST_F(NavigationControlPointTest, StaticAuthoringKeepsSavedBakeMetadataAndNumericValidation)
{
    auto& owner = m_scene.CreateGameObject("Patrol");
    auto& patrol = owner.AddComponent<scene::NavMeshPatrolComponent>();
    patrol.waypoints = {{4.0f, 5.0f, 6.0f}};
    patrol.pointRuntimeMode = scene::ControlPointRuntimeMode::BAKED_STATIC;
    const auto authored = scene::ResolveNavMeshPatrolPointAuthoring(
        m_scene, owner.GetID(), patrol, scene::ControlPointRuntimeMode::BAKED_STATIC);
    ASSERT_TRUE(authored.valid);
    ASSERT_EQ(authored.points.size(), 1u);
    patrol.bakedWaypoints = {{authored.points[0].stableId, authored.points[0].worldPosition,
        authored.points[0].waitTime, authored.points[0].speed}};
    patrol.bakedInputSignature = authored.inputSignature;
    patrol.bakedSourceMode = patrol.pointSourceMode;
    patrol.bakedAdapterVersion = scene::NAV_MESH_PATROL_ADAPTER_VERSION;
    patrol.bakeValid = true;
    const auto validPatrol = patrol;

    const auto expectInvalidBake = [&](scene::NavigationControlPointStatus expectedStatus) {
        const auto authoring = scene::ResolveNavMeshPatrolPoints(
            m_scene, owner.GetID(), patrol, scene::ControlPointReadContext::AUTHORING);
        EXPECT_FALSE(authoring.valid);
        EXPECT_TRUE(authoring.points.empty());
        EXPECT_TRUE(authoring.bakeStale);
        EXPECT_EQ(authoring.status, expectedStatus);
        const auto runtime = scene::ResolveNavMeshPatrolPoints(
            m_scene, owner.GetID(), patrol, scene::ControlPointReadContext::RUNTIME);
        EXPECT_FALSE(runtime.valid);
        EXPECT_TRUE(runtime.points.empty());
        EXPECT_EQ(runtime.status, expectedStatus);
    };

    patrol.bakeValid = false;
    expectInvalidBake(scene::NavigationControlPointStatus::MISSING_BAKE);
    patrol = validPatrol;
    patrol.bakedInputSignature = 0;
    expectInvalidBake(scene::NavigationControlPointStatus::STALE_BAKE);
    patrol = validPatrol;
    patrol.bakedAdapterVersion = scene::NAV_MESH_PATROL_ADAPTER_VERSION + 1;
    expectInvalidBake(scene::NavigationControlPointStatus::STALE_BAKE);
    patrol = validPatrol;
    patrol.bakedSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    expectInvalidBake(scene::NavigationControlPointStatus::STALE_BAKE);
    patrol = validPatrol;
    patrol.bakedWaypoints[0].worldPosition.x = std::numeric_limits<float>::quiet_NaN();
    expectInvalidBake(scene::NavigationControlPointStatus::INVALID_NUMERIC_DATA);
}

TEST_F(NavigationControlPointTest, StaticPointObjectBakeSurvivesReferenceRemapWithSameNumericRoute)
{
    auto& owner = m_scene.CreateGameObject("Patrol");
    auto& original = m_scene.CreateGameObject("Original Waypoint");
    auto& remapped = m_scene.CreateGameObject("Remapped Waypoint");
    ASSERT_NE(original.instanceId, remapped.instanceId);
    original.transform.position = {5.0f, 2.0f, -3.0f};
    remapped.transform.position = original.transform.position;
    original.AddComponent<scene::TargetPointComponent>();
    remapped.AddComponent<scene::TargetPointComponent>();
    auto& patrol = owner.AddComponent<scene::NavMeshPatrolComponent>();
    patrol.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    scene::NavMeshPatrolWaypointBinding binding;
    binding.id = 45;
    binding.target = {original.instanceId, original.GetID(), m_scene.GetRenderSceneGeneration()};
    binding.waitTime = 1.25f;
    binding.speed = 3.0f;
    patrol.waypointBindings.push_back(binding);
    scene::FlushWorldTransforms(m_scene);

    const auto authoring = scene::ResolveNavMeshPatrolPointAuthoring(
        m_scene, owner.GetID(), patrol, scene::ControlPointRuntimeMode::BAKED_STATIC);
    ASSERT_TRUE(authoring.valid);
    patrol.bakedWaypoints.push_back({authoring.points[0].stableId,
        authoring.points[0].worldPosition, authoring.points[0].waitTime, authoring.points[0].speed});
    patrol.bakedInputSignature = authoring.inputSignature;
    patrol.bakedSourceMode = patrol.pointSourceMode;
    patrol.bakedAdapterVersion = scene::NAV_MESH_PATROL_ADAPTER_VERSION;
    patrol.bakeValid = true;
    patrol.pointRuntimeMode = scene::ControlPointRuntimeMode::BAKED_STATIC;

    patrol.waypointBindings[0].target = {
        remapped.instanceId, remapped.GetID(), m_scene.GetRenderSceneGeneration()};
    const auto checked = scene::ResolveNavMeshPatrolPoints(
        m_scene, owner.GetID(), patrol, scene::ControlPointReadContext::AUTHORING);
    EXPECT_TRUE(checked.valid);
    EXPECT_FALSE(checked.bakeStale);
    EXPECT_EQ(checked.inputSignature, patrol.bakedInputSignature);

    patrol.mode = scene::NavMeshPatrolComponent::Mode::PING_PONG;
    const auto changedPolicy = scene::ResolveNavMeshPatrolPoints(
        m_scene, owner.GetID(), patrol, scene::ControlPointReadContext::AUTHORING);
    EXPECT_TRUE(changedPolicy.valid);
    EXPECT_FALSE(changedPolicy.bakeStale);
    EXPECT_EQ(changedPolicy.inputSignature, patrol.bakedInputSignature);
}

TEST_F(NavigationControlPointTest, PatrolKeepsCurrentBindingIdentityAcrossReorderAndRemoval)
{
    auto& owner = m_scene.CreateGameObject("Patrol");
    auto& first = m_scene.CreateGameObject("First");
    auto& second = m_scene.CreateGameObject("Second");
    first.transform.position = {2.0f, 0.0f, 1.0f};
    second.transform.position = {9.0f, 0.0f, 4.0f};
    first.AddComponent<scene::TargetPointComponent>();
    second.AddComponent<scene::TargetPointComponent>();
    auto& patrol = owner.AddComponent<scene::NavMeshPatrolComponent>();
    patrol.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    scene::NavMeshPatrolWaypointBinding firstBinding;
    firstBinding.id = 71;
    firstBinding.target = {first.instanceId, first.GetID(), m_scene.GetRenderSceneGeneration()};
    firstBinding.speed = 1.0f;
    scene::NavMeshPatrolWaypointBinding secondBinding;
    secondBinding.id = 93;
    secondBinding.target = {second.instanceId, second.GetID(), m_scene.GetRenderSceneGeneration()};
    secondBinding.speed = 2.0f;
    patrol.waypointBindings = {firstBinding, secondBinding};
    auto& agent = owner.AddComponent<scene::NavMeshAgentComponent>();
    scene::FlushWorldTransforms(m_scene);

    physics::World world;
    scene::NavMeshPatrolSystem system;
    SystemContext context{m_scene, world, nullptr, nullptr, 1.0f / 60.0f,
                          1.0f / 60.0f, false, false};
    system.Update(context);
    ASSERT_EQ(patrol.currentWaypointId, 71u);
    EXPECT_VEC3_NEAR(agent.destination, first.transform.worldPosition, testkit::kTolerance);
    const math::Vector3 selectedDestination = agent.destination;

    std::reverse(patrol.waypointBindings.begin(), patrol.waypointBindings.end());
    system.Update(context);
    EXPECT_EQ(patrol.currentWaypointId, 71u);
    EXPECT_EQ(patrol.currentIndex, 1u);
    EXPECT_VEC3_NEAR(agent.destination, selectedDestination, testkit::kTolerance);

    patrol.waypointBindings.erase(patrol.waypointBindings.begin() + 1);
    system.Update(context);
    EXPECT_EQ(patrol.currentWaypointId, 71u);
    EXPECT_FALSE(agent.hasDestination);
    EXPECT_VEC3_NEAR(agent.destination, selectedDestination, testkit::kTolerance);
    EXPECT_FLOAT_EQ(agent.maxSpeed, 1.0f);
}

TEST_F(NavigationControlPointTest, StationaryLiveBindingDoesNotRepathBeforeStartingItsWait)
{
    auto& owner = m_scene.CreateGameObject("Patrol");
    auto& target = m_scene.CreateGameObject("Waypoint");
    target.AddComponent<scene::TargetPointComponent>();
    auto& patrol = owner.AddComponent<scene::NavMeshPatrolComponent>();
    patrol.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    patrol.liveRepathInterval = 0.0f;
    patrol.liveRepathDistance = 0.0f;
    scene::NavMeshPatrolWaypointBinding binding;
    binding.id = 14;
    binding.target = {target.instanceId, target.GetID(), m_scene.GetRenderSceneGeneration()};
    binding.waitTime = 1.0f;
    patrol.waypointBindings.push_back(binding);
    auto& agent = owner.AddComponent<scene::NavMeshAgentComponent>();
    scene::FlushWorldTransforms(m_scene);

    physics::World world;
    scene::NavMeshPatrolSystem system;
    SystemContext context{m_scene, world, nullptr, nullptr, 1.0f / 60.0f,
                          1.0f / 60.0f, false, false};
    system.Update(context);
    ASSERT_TRUE(agent.hasDestination);
    ASSERT_TRUE(patrol.started);

    agent.destinationReached = true;
    agent.hasDestination = false;
    system.Update(context);
    EXPECT_TRUE(patrol.waiting);
    EXPECT_FLOAT_EQ(patrol.waitTimer, 1.0f);
}

TEST_F(NavigationControlPointTest, OffMeshPointObjectsResolveBothNumericEndpoints)
{
    auto& owner = m_scene.CreateGameObject("Link");
    auto& start = m_scene.CreateGameObject("Start");
    auto& end = m_scene.CreateGameObject("End");
    start.transform.position = {1.0f, 0.0f, 2.0f};
    end.transform.position = {9.0f, 3.0f, 4.0f};
    start.AddComponent<scene::TargetPointComponent>();
    end.AddComponent<scene::TargetPointComponent>();
    auto& link = owner.AddComponent<scene::NavMeshOffMeshLinkComponent>();
    link.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    link.startTarget = {start.instanceId, start.GetID(), m_scene.GetRenderSceneGeneration()};
    link.endTarget = {end.instanceId, end.GetID(), m_scene.GetRenderSceneGeneration()};
    scene::FlushWorldTransforms(m_scene);

    const auto snapshot = scene::ResolveNavMeshOffMeshLinkEndpoints(m_scene, owner.GetID(), link);
    ASSERT_TRUE(snapshot.valid);
    EXPECT_VEC3_NEAR(snapshot.startPoint, start.transform.worldPosition, testkit::kTolerance);
    EXPECT_VEC3_NEAR(snapshot.endPoint, end.transform.worldPosition, testkit::kTolerance);
    EXPECT_NE(snapshot.inputSignature, 0u);

    start.transform.position.x += 1.0f;
    scene::FlushWorldTransforms(m_scene);
    const auto moved = scene::ResolveNavMeshOffMeshLinkEndpoints(m_scene, owner.GetID(), link);
    ASSERT_TRUE(moved.valid);
    EXPECT_NE(moved.inputSignature, snapshot.inputSignature);
    EXPECT_FLOAT_EQ(moved.startPoint.x, 2.0f);

    link.traversalTime = std::numeric_limits<float>::quiet_NaN();
    const auto invalidTraversal = scene::ResolveNavMeshOffMeshLinkEndpoints(
        m_scene, owner.GetID(), link);
    EXPECT_FALSE(invalidTraversal.valid);
    EXPECT_EQ(invalidTraversal.status, scene::NavigationControlPointStatus::INVALID_NUMERIC_DATA);
}

TEST_F(NavigationControlPointTest, MissingAssignedAgentTargetHaltsWithoutFallingBackOrClearingGuid)
{
    auto& surfaceOwner = m_scene.CreateGameObject("Surface");
    auto& surface = surfaceOwner.AddComponent<scene::NavMeshSurfaceComponent>();
    scene::NavMeshPolygon polygon;
    polygon.vertices = {{-10.0f, 0.0f, -10.0f}, {10.0f, 0.0f, -10.0f},
                        {10.0f, 0.0f, 10.0f}, {-10.0f, 0.0f, 10.0f}};
    surface.navMesh.polygons.push_back(polygon);

    auto& agentOwner = m_scene.CreateGameObject("Agent");
    auto& decoy = m_scene.CreateGameObject("Stale Cache Decoy");
    auto& agent = agentOwner.AddComponent<scene::NavMeshAgentComponent>();
    agent.target.guid = "missing-agent-target";
    agent.target.id = decoy.GetID();
    agent.target.sceneGeneration = m_scene.GetRenderSceneGeneration();
    agent.hasDestination = true;
    agent.destination = {4.0f, 0.0f, 6.0f};
    agent.path = {{3.0f, 0.0f, 5.0f}};

    physics::World world;
    scene::NavigationSystem system;
    SystemContext context{m_scene, world, nullptr, nullptr, 1.0f / 60.0f,
                          1.0f / 60.0f, false, false};
    system.Update(context);

    EXPECT_TRUE(agent.target.IsAssigned());
    EXPECT_EQ(agent.target.guid, "missing-agent-target");
    EXPECT_FALSE(agent.hasDestination);
    EXPECT_TRUE(agent.path.empty());
}

TEST_F(NavigationControlPointTest, LivePatrolRejectsTargetWithinItsOwnHierarchyAndStops)
{
    auto& owner = m_scene.CreateGameObject("Patrol");
    auto& point = m_scene.CreateGameObject("Owned Waypoint");
    point.SetParent(owner);
    point.AddComponent<scene::TargetPointComponent>();
    auto& patrol = owner.AddComponent<scene::NavMeshPatrolComponent>();
    patrol.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    scene::NavMeshPatrolWaypointBinding binding;
    binding.id = 4;
    binding.target = {point.instanceId, point.GetID(), m_scene.GetRenderSceneGeneration()};
    patrol.waypointBindings.push_back(binding);
    auto& agent = owner.AddComponent<scene::NavMeshAgentComponent>();
    agent.hasDestination = true;
    agent.destination = {4.0f, 0.0f, 7.0f};
    patrol.started = true;
    scene::FlushWorldTransforms(m_scene);

    const auto snapshot = scene::ResolveNavMeshPatrolPoints(
        m_scene, owner.GetID(), patrol, scene::ControlPointReadContext::RUNTIME);
    EXPECT_FALSE(snapshot.valid);
    EXPECT_EQ(snapshot.status, scene::NavigationControlPointStatus::SELF_REFERENTIAL_TARGET);

    physics::World world;
    scene::NavMeshPatrolSystem system;
    SystemContext context{m_scene, world, nullptr, nullptr, 1.0f / 60.0f,
                          1.0f / 60.0f, false, false};
    system.Update(context);
    EXPECT_FALSE(agent.hasDestination);
    EXPECT_FALSE(agent.target.IsAssigned());
}

TEST_F(NavigationControlPointTest, StaticPatrolUsesCapturedPointInsideItsOwnHierarchy)
{
    auto& owner = m_scene.CreateGameObject("Patrol");
    auto& point = m_scene.CreateGameObject("Baked Child Waypoint");
    point.SetParent(owner);
    point.transform.position = {2.0f, 1.0f, 4.0f};
    point.AddComponent<scene::TargetPointComponent>();
    auto& patrol = owner.AddComponent<scene::NavMeshPatrolComponent>();
    patrol.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    scene::NavMeshPatrolWaypointBinding binding;
    binding.id = 12;
    binding.target = {point.instanceId, point.GetID(), m_scene.GetRenderSceneGeneration()};
    patrol.waypointBindings.push_back(binding);
    patrol.pointRuntimeMode = scene::ControlPointRuntimeMode::BAKED_STATIC;
    scene::FlushWorldTransforms(m_scene);

    const auto authoring = scene::ResolveNavMeshPatrolPointAuthoring(
        m_scene, owner.GetID(), patrol, scene::ControlPointRuntimeMode::BAKED_STATIC);
    ASSERT_TRUE(authoring.valid);
    ASSERT_EQ(authoring.points.size(), 1u);
    const math::Vector3 capturedPosition = authoring.points[0].worldPosition;
    patrol.bakedWaypoints.push_back({authoring.points[0].stableId,
        capturedPosition, authoring.points[0].waitTime, authoring.points[0].speed});
    patrol.bakedInputSignature = authoring.inputSignature;
    patrol.bakedSourceMode = patrol.pointSourceMode;
    patrol.bakedAdapterVersion = scene::NAV_MESH_PATROL_ADAPTER_VERSION;
    patrol.bakeValid = true;
    point.transform.position = {50.0f, 10.0f, -20.0f};
    scene::FlushWorldTransforms(m_scene);

    const auto runtime = scene::ResolveNavMeshPatrolPoints(
        m_scene, owner.GetID(), patrol, scene::ControlPointReadContext::RUNTIME);
    ASSERT_TRUE(runtime.valid);
    ASSERT_EQ(runtime.points.size(), 1u);
    EXPECT_VEC3_NEAR(runtime.points[0].worldPosition, capturedPosition, testkit::kTolerance);

    patrol.waypointBindings[0].waitTime = 1.5f;
    patrol.waypointBindings[0].speed = 2.75f;
    const auto edited = scene::ResolveNavMeshPatrolPoints(
        m_scene, owner.GetID(), patrol, scene::ControlPointReadContext::AUTHORING);
    ASSERT_TRUE(edited.valid);
    ASSERT_EQ(edited.points.size(), 1u);
    EXPECT_TRUE(edited.bakeStale);
    EXPECT_EQ(edited.status, scene::NavigationControlPointStatus::STALE_BAKE);
    EXPECT_EQ(edited.points[0].stableId, binding.id);
    EXPECT_VEC3_NEAR(edited.points[0].worldPosition, point.transform.worldPosition, testkit::kTolerance);
    EXPECT_FLOAT_EQ(edited.points[0].waitTime, 1.5f);
    EXPECT_FLOAT_EQ(edited.points[0].speed, 2.75f);
    EXPECT_NE(edited.inputSignature, patrol.bakedInputSignature);

    const auto unchangedRuntime = scene::ResolveNavMeshPatrolPoints(
        m_scene, owner.GetID(), patrol, scene::ControlPointReadContext::RUNTIME);
    ASSERT_TRUE(unchangedRuntime.valid);
    ASSERT_EQ(unchangedRuntime.points.size(), 1u);
    EXPECT_VEC3_NEAR(unchangedRuntime.points[0].worldPosition, capturedPosition, testkit::kTolerance);
    EXPECT_FLOAT_EQ(unchangedRuntime.points[0].waitTime, 0.0f);
    EXPECT_FLOAT_EQ(unchangedRuntime.points[0].speed, 0.0f);
}

TEST_F(NavigationControlPointTest, AgentTargetWithinItsOwnHierarchyHaltsAndKeepsReference)
{
    auto& surfaceOwner = m_scene.CreateGameObject("Surface");
    auto& surface = surfaceOwner.AddComponent<scene::NavMeshSurfaceComponent>();
    scene::NavMeshPolygon polygon;
    polygon.vertices = {{-10.0f, 0.0f, -10.0f}, {10.0f, 0.0f, -10.0f},
                        {10.0f, 0.0f, 10.0f}, {-10.0f, 0.0f, 10.0f}};
    surface.navMesh.polygons.push_back(polygon);

    auto& owner = m_scene.CreateGameObject("Agent");
    auto& child = m_scene.CreateGameObject("Child Goal");
    child.SetParent(owner);
    auto& agent = owner.AddComponent<scene::NavMeshAgentComponent>();
    agent.target = scene::EntityRef(child.GetID(), child.instanceId, m_scene.GetRenderSceneGeneration());
    agent.hasDestination = true;
    agent.destination = {4.0f, 0.0f, 7.0f};
    agent.path = {{3.0f, 0.0f, 5.0f}};
    scene::FlushWorldTransforms(m_scene);

    physics::World world;
    scene::NavigationSystem system;
    SystemContext context{m_scene, world, nullptr, nullptr, 1.0f / 60.0f,
                          1.0f / 60.0f, false, false};
    system.Update(context);

    EXPECT_TRUE(agent.target.IsAssigned());
    EXPECT_EQ(agent.target.guid, child.instanceId);
    EXPECT_FALSE(agent.hasDestination);
    EXPECT_TRUE(agent.path.empty());
}

} /// @note namespace
} /// @note namespace fbzz::tests
