/// @file    ManagedRenderComponentsTests.cpp
/// @brief   描画 bridge の検証、native birth と実 CoreCLR facade・保存・catalog を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#include <Engine/Scene/ScriptComponent.hpp>
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/Components/NavMeshPatrolComponent.hpp>
#include <Engine/Scene/Components/TargetPointComponent.hpp>
#include <Engine/Scene/NavigationControlPoints.hpp>
#include <Engine/Scene/PresentationControlPoints.hpp>
#include <Engine/Scene/Components/TrailComponent.hpp>
#include <Engine/Scene/Components/MeshTrailComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/ProceduralMeshComponent.hpp>
#include <Engine/Scene/Managed/ManagedScriptAdapter.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include "../../../Engine/src/Scene/Managed/ManagedRenderComponentServices.hpp"
#include "../../../Engine/src/Scene/Managed/ManagedRenderTransfers.hpp"
#include "../../../Engine/src/Scene/Managed/ManagedServicesBridge.hpp"
#include "../../../Engine/src/Scene/Managed/ManagedObjectControls.hpp"
#include <array>
#include <limits>

namespace fbzz::tests {
using RenderStatus = scene::ManagedBridgeStatus;
using RenderOp = scene::ManagedBridgeOp;
using RenderKind = scene::ManagedNativeComponentKind;
using RenderControls = scene::managed_detail::ManagedObjectControls;
class ManagedRenderComponentServicesTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    scene::Script m_view;
    scene::GameObject* m_owner = nullptr;
    scene::managed_detail::ManagedServiceResult m_result;
    bool m_previousPlay = false;
    void SetUp() override
    {
        EngineFixture::SetUp(); m_previousPlay = scene::Script::IsInPlayMode(); scene::Script::SetInPlayMode(true);
        m_owner = &m_scene.CreateGameObject("Render row"); m_view.SetContext(&m_scene, m_owner);
    }
    void TearDown() override
    {
        m_view.SetContext(nullptr, nullptr); m_scene.Clear(); scene::Script::SetInPlayMode(m_previousPlay); EngineFixture::TearDown();
    }
    RenderStatus Run(RenderOp op, uint32_t method, bool read, int64_t integer = 0,
        std::span<const float> values = {}, std::span<const std::string> text = {})
    {
        scene::ManagedBridgeRequest request; request.op = op; request.index = method; request.flags = read ? 1 : 0;
        return scene::managed_detail::RunRenderComponentServices(m_scene, m_view, *m_owner, nullptr,
            request, integer, values, text, m_result);
    }
};

TEST_F(ManagedRenderComponentServicesTest, RenderingRowsUseBirthsAndPendingRemovalAboveOldBoundary)
{
    for (const auto kind : std::array<RenderKind, 7>{RenderKind::LINE_RENDERER, RenderKind::SPRITE_RENDERER, RenderKind::SORTING_GROUP,
        RenderKind::TRAIL, RenderKind::MESH_TRAIL, RenderKind::MESH_RENDERER, RenderKind::SKINNED_MESH_RENDERER}) {
        bool exists = false; uint64_t first = 0, pending = 99, replacement = 0;
        const auto index = static_cast<uint32_t>(kind);
        ASSERT_EQ(RenderControls::Component(*m_owner, index, RenderOp::COMPONENT_ADD, exists, &first), RenderStatus::OK);
        ASSERT_TRUE(exists); ASSERT_NE(first, 0u);
        ASSERT_EQ(RenderControls::Component(*m_owner, index, RenderOp::COMPONENT_REMOVE, exists), RenderStatus::OK);
        EXPECT_TRUE(RenderControls::IsPendingComponentRemoval(*m_owner, index));
        ASSERT_EQ(RenderControls::Component(*m_owner, index, RenderOp::COMPONENT_READ, exists, &pending), RenderStatus::OK);
        EXPECT_FALSE(exists); EXPECT_EQ(pending, 0u);
        RenderControls::FlushScriptRemovals(m_scene);
        ASSERT_EQ(RenderControls::Component(*m_owner, index, RenderOp::COMPONENT_ADD, exists, &replacement), RenderStatus::OK);
        EXPECT_TRUE(exists); EXPECT_NE(first, replacement);
    }
}

TEST_F(ManagedRenderComponentServicesTest, NativeLineProxyRejectsUnknownEnumsAndKeepsPointOutputOnFailure)
{
    math::Vector3 point{91, 92, 93};
    EXPECT_EQ(m_view.gameplay.GetLinePointCount(), 0u);
    EXPECT_FALSE(m_view.gameplay.TryGetLinePoint(0, point));
    EXPECT_VEC3_NEAR(point, (math::Vector3{91, 92, 93}), 0.00001f);
    EXPECT_FALSE(m_view.gameplay.SetLineLoop(true));
    auto& line = m_owner->AddComponent<scene::LineRendererComponent>();
    ASSERT_TRUE(m_view.gameplay.SetLine({1, 2, 3}, {4, 5, 6}, false));
    ASSERT_TRUE(m_view.gameplay.SetLineLoop(true)); ASSERT_TRUE(m_view.gameplay.SetLineBillboard(false));
    ASSERT_TRUE(m_view.gameplay.SetLineShape(scene::LineShape::Tube)); ASSERT_TRUE(m_view.gameplay.SetLineSpace(scene::LineSpace::World));
    ASSERT_TRUE(m_view.gameplay.SetLineRadialSegments(32)); ASSERT_TRUE(m_view.gameplay.SetLineSorting(INT32_MIN, INT32_MAX));
    EXPECT_TRUE(line.loop); EXPECT_FALSE(line.billboard); EXPECT_EQ(line.shape, scene::LineShape::Tube);
    EXPECT_EQ(line.space, scene::LineSpace::World); EXPECT_EQ(line.radialSegments, 32);
    EXPECT_EQ(line.sortingLayer, INT32_MIN); EXPECT_EQ(line.orderInLayer, INT32_MAX);
    EXPECT_FALSE(m_view.gameplay.SetLineShape(static_cast<scene::LineShape>(2)));
    EXPECT_FALSE(m_view.gameplay.SetLineSpace(static_cast<scene::LineSpace>(-1)));
    EXPECT_FALSE(m_view.gameplay.SetLineRadialSegments(2)); EXPECT_FALSE(m_view.gameplay.SetLineRadialSegments(33));
    EXPECT_EQ(line.shape, scene::LineShape::Tube); EXPECT_EQ(line.space, scene::LineSpace::World); EXPECT_EQ(line.radialSegments, 32);
    EXPECT_EQ(m_view.gameplay.GetLinePointCount(), 2u); EXPECT_FALSE(m_view.gameplay.TryGetLinePoint(2, point));
    EXPECT_VEC3_NEAR(point, (math::Vector3{91, 92, 93}), 0.00001f);
    ASSERT_TRUE(m_view.gameplay.TryGetLinePoint(1, point)); EXPECT_VEC3_NEAR(point, (math::Vector3{4, 5, 6}), 0.00001f);
}

TEST_F(ManagedRenderComponentServicesTest, NativeLineProxyReadsPointObjectsAndNumericWritesSelectInlineWithoutDeletingTargets)
{
    auto& line = m_owner->AddComponent<scene::LineRendererComponent>();
    line.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    line.pointRuntimeMode = scene::ControlPointRuntimeMode::LIVE;
    line.space = scene::LineSpace::World;
    line.pointBindings = {{13, {}}, {4, {}}};
    line.bakeValid = true;
    line.bakedInputSignature = 91;
    line.bakedSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    line.bakedAdapterVersion = scene::LINE_CONTROL_POINT_ADAPTER_VERSION;
    line.bakedSpace = scene::LineSpace::World;
    line.bakedPoints = {{13, {21.0f, 3.0f, 5.0f}}, {4, {22.0f, 4.0f, 6.0f}}};
    auto& first = m_scene.CreateGameObject("Line point A");
    auto& second = m_scene.CreateGameObject("Line point B");
    first.transform.position = {1.0f, 2.0f, 3.0f};
    second.transform.position = {4.0f, 5.0f, 6.0f};
    first.AddComponent<scene::TargetPointComponent>(); second.AddComponent<scene::TargetPointComponent>();
    line.pointBindings[0].target = {first.instanceId, first.GetID(), m_scene.GetRenderSceneGeneration()};
    line.pointBindings[1].target = {second.instanceId, second.GetID(), m_scene.GetRenderSceneGeneration()};
    const size_t objectCount = m_scene.GameObjectCount();

    EXPECT_EQ(m_view.gameplay.GetLinePointCount(), 2u);
    math::Vector3 point{};
    ASSERT_TRUE(m_view.gameplay.TryGetLinePoint(1, point));
    EXPECT_VEC3_NEAR(point, (math::Vector3{4.0f, 5.0f, 6.0f}), testkit::kTolerance);
    line.pointRuntimeMode = scene::ControlPointRuntimeMode::BAKED_STATIC;
    first.transform.position = {101.0f, 102.0f, 103.0f};
    second.transform.position = {104.0f, 105.0f, 106.0f};
    ASSERT_TRUE(m_view.gameplay.TryGetLinePoint(0, point));
    EXPECT_VEC3_NEAR(point, (math::Vector3{21.0f, 3.0f, 5.0f}), testkit::kTolerance);
    scene::Script::SetInPlayMode(false);
    ASSERT_TRUE(m_view.gameplay.TryGetLinePoint(0, point));
    EXPECT_VEC3_NEAR(point, (math::Vector3{21.0f, 3.0f, 5.0f}), testkit::kTolerance);
    const auto authoring = scene::ResolveLineControlPoints(m_scene, m_owner->GetID(), line,
        scene::ControlPointReadContext::AUTHORING);
    EXPECT_TRUE(authoring.bakeStale);
    line.pointRuntimeMode = scene::ControlPointRuntimeMode::LIVE;
    ASSERT_TRUE(m_view.gameplay.TryGetLinePoint(0, point));
    EXPECT_VEC3_NEAR(point, (math::Vector3{101.0f, 102.0f, 103.0f}), testkit::kTolerance);
    scene::Script::SetInPlayMode(true);
    const std::array<math::Vector3, 2> numericPoints{{{8.0f, 7.0f, 6.0f}, {5.0f, 4.0f, 3.0f}}};

    ASSERT_TRUE(m_view.gameplay.SetLinePoints(numericPoints, false));

    EXPECT_EQ(line.pointSourceMode, scene::ControlPointSourceMode::INLINE);
    EXPECT_EQ(line.pointRuntimeMode, scene::ControlPointRuntimeMode::LIVE);
    EXPECT_EQ(line.space, scene::LineSpace::Local);
    EXPECT_EQ(line.points, std::vector<math::Vector3>(numericPoints.begin(), numericPoints.end()));
    ASSERT_EQ(line.pointBindings.size(), 2u);
    EXPECT_EQ(line.pointBindings[0].target.guid, first.instanceId);
    EXPECT_EQ(line.pointBindings[1].target.guid, second.instanceId);
    EXPECT_TRUE(line.bakeValid);
    EXPECT_EQ(line.bakedInputSignature, 91u);
    EXPECT_EQ(line.bakedPoints.size(), 2u);
    EXPECT_EQ(m_scene.GameObjectCount(), objectCount);
    EXPECT_NE(first.GetComponent<scene::TargetPointComponent>(), nullptr);
    EXPECT_EQ(m_view.gameplay.GetLinePointCount(), 2u);
}

TEST_F(ManagedRenderComponentServicesTest, NativePatrolProxyReadsAndMaterializesPointObjectsForNumericWrites)
{
    auto& patrol = m_owner->AddComponent<scene::NavMeshPatrolComponent>();
    patrol.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    patrol.pointRuntimeMode = scene::ControlPointRuntimeMode::LIVE;
    patrol.waypointBindings = {{15, {}, 0.25f, 2.0f}, {6, {}, 0.75f, 4.0f}};
    auto& first = m_scene.CreateGameObject("Patrol point A");
    auto& second = m_scene.CreateGameObject("Patrol point B");
    first.transform.position = {1.0f, 2.0f, 3.0f};
    second.transform.position = {4.0f, 5.0f, 6.0f};
    first.AddComponent<scene::TargetPointComponent>(); second.AddComponent<scene::TargetPointComponent>();
    patrol.waypointBindings[0].target = {first.instanceId, first.GetID(), m_scene.GetRenderSceneGeneration()};
    patrol.waypointBindings[1].target = {second.instanceId, second.GetID(), m_scene.GetRenderSceneGeneration()};
    const size_t objectCount = m_scene.GameObjectCount();

    EXPECT_EQ(m_view.patrol.GetWaypointCount(), 2u);
    math::Vector3 point{19.0f, 18.0f, 17.0f};
    ASSERT_TRUE(m_view.patrol.GetWaypoint(1, point));
    EXPECT_VEC3_NEAR(point, (math::Vector3{4.0f, 5.0f, 6.0f}), testkit::kTolerance);
    patrol.pointRuntimeMode = scene::ControlPointRuntimeMode::BAKED_STATIC;
    patrol.bakedSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    patrol.bakedAdapterVersion = scene::NAV_MESH_PATROL_ADAPTER_VERSION;
    patrol.bakedInputSignature = 137;
    patrol.bakeValid = true;
    patrol.bakedWaypoints = {{15, {21.0f, 3.0f, 5.0f}, 1.25f, 8.0f},
        {6, {22.0f, 4.0f, 6.0f}, 2.5f, 9.0f}};
    ASSERT_TRUE(m_view.patrol.GetWaypoint(1, point));
    EXPECT_VEC3_NEAR(point, (math::Vector3{22.0f, 4.0f, 6.0f}), testkit::kTolerance);
    scene::Script::SetInPlayMode(false);
    ASSERT_TRUE(m_view.patrol.GetWaypoint(1, point));
    EXPECT_VEC3_NEAR(point, (math::Vector3{4.0f, 5.0f, 6.0f}), testkit::kTolerance);
    scene::Script::SetInPlayMode(true);
    EXPECT_FALSE(m_view.patrol.SetWaypoint(2, {9.0f, 8.0f, 7.0f}));
    EXPECT_EQ(patrol.pointSourceMode, scene::ControlPointSourceMode::POINT_OBJECTS);
    EXPECT_TRUE(patrol.waypoints.empty());

    ASSERT_TRUE(m_view.patrol.SetWaypoint(0, {9.0f, 8.0f, 7.0f}));

    EXPECT_EQ(patrol.pointSourceMode, scene::ControlPointSourceMode::INLINE);
    EXPECT_EQ(patrol.pointRuntimeMode, scene::ControlPointRuntimeMode::LIVE);
    ASSERT_EQ(patrol.waypoints.size(), 2u);
    EXPECT_VEC3_NEAR(patrol.waypoints[0], (math::Vector3{9.0f, 8.0f, 7.0f}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(patrol.waypoints[1], (math::Vector3{22.0f, 4.0f, 6.0f}), testkit::kTolerance);
    EXPECT_EQ(patrol.waypointWaitTimes, (std::vector<float>{1.25f, 2.5f}));
    EXPECT_EQ(patrol.waypointSpeeds, (std::vector<float>{8.0f, 9.0f}));
    EXPECT_EQ(patrol.waypointBindings.size(), 2u);
    EXPECT_EQ(m_scene.GameObjectCount(), objectCount);
    EXPECT_NE(first.GetComponent<scene::TargetPointComponent>(), nullptr);

    patrol.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    patrol.pointRuntimeMode = scene::ControlPointRuntimeMode::LIVE;
    m_view.patrol.AddWaypoint({8.0f, 7.0f, 6.0f}, 1.0f, 3.0f);
    EXPECT_EQ(patrol.pointSourceMode, scene::ControlPointSourceMode::INLINE);
    ASSERT_EQ(patrol.waypoints.size(), 3u);
    EXPECT_VEC3_NEAR(patrol.waypoints[0], (math::Vector3{1.0f, 2.0f, 3.0f}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(patrol.waypoints[1], (math::Vector3{4.0f, 5.0f, 6.0f}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(patrol.waypoints[2], (math::Vector3{8.0f, 7.0f, 6.0f}), testkit::kTolerance);
    EXPECT_EQ(patrol.waypointBindings.size(), 2u);
    EXPECT_EQ(m_scene.GameObjectCount(), objectCount);
    m_view.patrol.ClearWaypoints();
    EXPECT_EQ(patrol.pointSourceMode, scene::ControlPointSourceMode::INLINE);
    EXPECT_EQ(patrol.pointRuntimeMode, scene::ControlPointRuntimeMode::LIVE);
    EXPECT_TRUE(patrol.waypoints.empty());
    EXPECT_EQ(patrol.waypointBindings.size(), 2u);
    EXPECT_EQ(m_scene.GameObjectCount(), objectCount);
}

TEST_F(ManagedRenderComponentServicesTest, ManagedLineBatchCommitSelectsInlineLiveAndRetainsPointMetadata)
{
    auto& line = m_owner->AddComponent<scene::LineRendererComponent>();
    line.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    line.pointRuntimeMode = scene::ControlPointRuntimeMode::BAKED_STATIC;
    line.pointBindings = {{17, {}}};
    auto& target = m_scene.CreateGameObject("Retained line marker");
    target.AddComponent<scene::TargetPointComponent>();
    line.pointBindings[0].target = {target.instanceId, target.GetID(), m_scene.GetRenderSceneGeneration()};
    line.bakeValid = true;
    line.bakedInputSignature = 481;
    line.bakedPoints = {{17, {3.0f, 2.0f, 1.0f}}};
    const size_t objectCount = m_scene.GameObjectCount();
    scene::managed_detail::ManagedRenderTransfers transfers;
    scene::ManagedBridgeRequest request;
    request.op = RenderOp::SERVICE_LINE;
    request.callId = 41;
    request.ownerId = 11;
    request.instanceId = 12;
    request.generation = 13;
    request.integer = 29;
    scene::managed_detail::ManagedServiceResult result;
    const std::array<float, 1> worldSpace{1.0f};
    request.index = 50;

    ASSERT_EQ(transfers.Run(m_scene, *m_owner, m_view, request, 2, worldSpace, {}, result), RenderStatus::OK);
    const int64_t token = result.integer;
    const std::array<float, 6> points{1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
    const std::array<std::string, 1> offset{"0"};
    request.index = 51;
    ASSERT_EQ(transfers.Run(m_scene, *m_owner, m_view, request, token, points, offset, result), RenderStatus::OK);
    request.index = 52;

    ASSERT_EQ(transfers.Run(m_scene, *m_owner, m_view, request, token, {}, {}, result), RenderStatus::OK);

    EXPECT_EQ(line.pointSourceMode, scene::ControlPointSourceMode::INLINE);
    EXPECT_EQ(line.pointRuntimeMode, scene::ControlPointRuntimeMode::LIVE);
    EXPECT_EQ(line.space, scene::LineSpace::World);
    EXPECT_EQ(line.points, (std::vector<math::Vector3>{{1.0f, 2.0f, 3.0f}, {4.0f, 5.0f, 6.0f}}));
    ASSERT_EQ(line.pointBindings.size(), 1u);
    EXPECT_EQ(line.pointBindings[0].target.guid, target.instanceId);
    EXPECT_TRUE(line.bakeValid);
    EXPECT_EQ(line.bakedInputSignature, 481u);
    EXPECT_EQ(line.bakedPoints.size(), 1u);
    EXPECT_EQ(m_scene.GameObjectCount(), objectCount);
    EXPECT_NE(target.GetComponent<scene::TargetPointComponent>(), nullptr);
}

TEST_F(ManagedRenderComponentServicesTest, LinePairsRejectMalformedPacketsAndPreserveScalarPrecision)
{
    auto& line = m_owner->AddComponent<scene::LineRendererComponent>();
    const std::array<float, 8> colors{1, 0, 0, 0.75f, 0, 0, 1, 0.125f};
    ASSERT_EQ(Run(RenderOp::SERVICE_LINE, 29, false, 0, colors), RenderStatus::OK);
    EXPECT_FLOAT_EQ(line.startColor.w, 0.75f); EXPECT_FLOAT_EQ(line.endColor.w, 0.125f);
    const auto before = line.endColor;
    EXPECT_EQ(Run(RenderOp::SERVICE_LINE, 29, false, 0, std::span(colors).first(7)), RenderStatus::INVALID_ARGUMENT);
    EXPECT_FLOAT_EQ(line.endColor.w, before.w);
    const std::array<float, 2> widths{0.3f, 0.8f}, negative{1, -1};
    ASSERT_EQ(Run(RenderOp::SERVICE_LINE, 30, false, 0, widths), RenderStatus::OK);
    EXPECT_EQ(Run(RenderOp::SERVICE_LINE, 30, false, 0, negative), RenderStatus::INVALID_ARGUMENT);
    EXPECT_FLOAT_EQ(line.startWidth, 0.3f); EXPECT_FLOAT_EQ(line.endWidth, 0.8f);
    EXPECT_EQ(Run(RenderOp::SERVICE_LINE, 23, false, INT32_MIN), RenderStatus::OK);
    EXPECT_EQ(Run(RenderOp::SERVICE_LINE, 25, false, INT32_MAX), RenderStatus::OK);
    EXPECT_EQ(line.sortingLayer, INT32_MIN); EXPECT_EQ(line.orderInLayer, INT32_MAX);
    EXPECT_EQ(Run(RenderOp::SERVICE_LINE, 21, false, 2), RenderStatus::INVALID_ARGUMENT);
    EXPECT_EQ(line.radialSegments, 8);
    const std::array<float, 1> invalid{std::numeric_limits<float>::infinity()};
    EXPECT_EQ(Run(RenderOp::SERVICE_LINE, 7, false, 0, invalid), RenderStatus::INVALID_ARGUMENT);
    EXPECT_FLOAT_EQ(line.startWidth, 0.3f);
}

TEST_F(ManagedRenderComponentServicesTest, TrailStopKeepsSamplesWhenFadeWasRequestedAndRequiresPlay)
{
    auto& trail = m_owner->AddComponent<scene::TrailComponent>(); trail.ringCount = 3;
    auto& meshTrail = m_owner->AddComponent<scene::MeshTrailComponent>(); meshTrail.sampleCount = 2;
    const std::array<float, 1> fade{0}, clear{1};
    ASSERT_EQ(Run(RenderOp::SERVICE_TRAIL, 1, false, 0, fade), RenderStatus::OK);
    EXPECT_FALSE(trail.enabled); EXPECT_FALSE(trail.clearOnDisable); EXPECT_EQ(trail.ringCount, 3);
    ASSERT_EQ(Run(RenderOp::SERVICE_MESH_TRAIL, 1, false, 0, fade), RenderStatus::OK);
    EXPECT_FALSE(meshTrail.enabled); EXPECT_FALSE(meshTrail.clearOnDisable); EXPECT_FALSE(meshTrail.clearRequested);
    ASSERT_EQ(Run(RenderOp::SERVICE_TRAIL, 1, false, 0, clear), RenderStatus::OK); EXPECT_EQ(trail.ringCount, 0);
    ASSERT_EQ(Run(RenderOp::SERVICE_MESH_TRAIL, 5, false), RenderStatus::OK); EXPECT_TRUE(meshTrail.clearRequested);
    scene::Script::SetInPlayMode(false);
    EXPECT_EQ(Run(RenderOp::SERVICE_TRAIL, 1, false, 1, fade), RenderStatus::INVALID_SCOPE); EXPECT_FALSE(trail.enabled);
    ASSERT_EQ(Run(RenderOp::SERVICE_TRAIL, 0, true), RenderStatus::OK); EXPECT_EQ(m_result.integer, 0);
}

TEST_F(ManagedRenderComponentServicesTest, StaticMeshClearRemovesShapeWithoutChangingRendererEnable)
{
    renderer::Mesh retained;
    auto& mesh = m_owner->AddComponent<scene::MeshRenderer>();
    mesh.mesh = &retained; mesh.meshPath = "primitive:sphere"; mesh.meshPathDirty = true;
    ASSERT_EQ(Run(RenderOp::SERVICE_MESH, 9, false), RenderStatus::OK);
    EXPECT_EQ(mesh.mesh, nullptr); EXPECT_TRUE(mesh.meshPath.empty()); EXPECT_TRUE(mesh.enabled);
}

TEST_F(ManagedRenderComponentServicesTest, FreshStaticMeshPrimitiveAndModelClearDoNotRequireAProceduralRow)
{
    auto& mesh = m_owner->AddComponent<scene::MeshRenderer>();
    ASSERT_EQ(m_owner->GetComponent<scene::ProceduralMeshComponent>(), nullptr);
    ASSERT_TRUE(m_scene.GetEntities<scene::ProceduralMeshComponent>().empty());
    ASSERT_EQ(Run(RenderOp::SERVICE_MESH, 6, false, 0), RenderStatus::OK);
    EXPECT_EQ(mesh.meshPath, "primitive:cube");
    EXPECT_TRUE(mesh.meshPathDirty);
    EXPECT_TRUE(mesh.enabled);

    auto& other = m_scene.CreateGameObject("Retained procedural mesh");
    auto& procedural = other.AddComponent<scene::ProceduralMeshComponent>();
    procedural.enabled = false;
    const auto lifetime = m_scene.GetComponentLifetime<scene::ProceduralMeshComponent>(other.GetID());
    ASSERT_NE(lifetime, 0u);
    ASSERT_EQ(Run(RenderOp::SERVICE_MESH, 6, false, 1), RenderStatus::OK);
    EXPECT_EQ(mesh.meshPath, "primitive:sphere");
    const std::array<std::string, 1> emptyModel{std::string{}};
    ASSERT_EQ(Run(RenderOp::SERVICE_MESH, 5, false, 0, {}, emptyModel), RenderStatus::OK);
    EXPECT_EQ(m_result.integer, 1);
    EXPECT_TRUE(mesh.meshPath.empty());
    EXPECT_TRUE(mesh.enabled);
    EXPECT_EQ(m_owner->GetComponent<scene::ProceduralMeshComponent>(), nullptr);
    EXPECT_EQ(other.GetComponent<scene::ProceduralMeshComponent>(), &procedural);
    EXPECT_FALSE(procedural.enabled);
    EXPECT_EQ(m_scene.GetComponentLifetime<scene::ProceduralMeshComponent>(other.GetID()), lifetime);
    EXPECT_EQ(m_scene.GetEntities<scene::ProceduralMeshComponent>().size(), 1u);
}

TEST_F(ManagedRenderComponentServicesTest, OptionalRemovalOfAbsentInvalidAndExpiredEntitiesPreservesExistingRows)
{
    auto& other = m_scene.CreateGameObject("Live procedural mesh");
    auto& procedural = other.AddComponent<scene::ProceduralMeshComponent>();
    const auto lifetime = m_scene.GetComponentLifetime<scene::ProceduralMeshComponent>(other.GetID());
    auto& expired = m_scene.CreateGameObject("Expired");
    const auto expiredId = expired.GetID();
    ASSERT_TRUE(m_scene.DestroyGameObject(expiredId));
    m_owner->RemoveComponent<scene::ProceduralMeshComponent>();
    m_scene.RemoveComponent<scene::ProceduralMeshComponent>(scene::EntityID::INVALID);
    m_scene.RemoveComponent<scene::ProceduralMeshComponent>({scene::EntityID::INVALID_INDEX - 1, 1});
    m_scene.RemoveComponent<scene::ProceduralMeshComponent>(expiredId);
    EXPECT_EQ(other.GetComponent<scene::ProceduralMeshComponent>(), &procedural);
    EXPECT_EQ(m_scene.GetComponentLifetime<scene::ProceduralMeshComponent>(other.GetID()), lifetime);
    EXPECT_EQ(m_scene.GetEntities<scene::ProceduralMeshComponent>().size(), 1u);
}

class ManagedRenderComponentsRuntimeTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    scene::GameObject* m_owner = nullptr;
    scene::ManagedScriptAdapter* m_adapter = nullptr;
    bool m_previousPlay = false;
    void SetUp() override
    {
        EngineFixture::SetUp(); m_previousPlay = scene::Script::IsInPlayMode(); scene::Script::SetInPlayMode(true);
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        ASSERT_TRUE(runtime.Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY)) << runtime.LastDiagnostic();
        ASSERT_TRUE(runtime.Load(FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
        m_owner = &m_scene.CreateGameObject("CLR Render");
        m_owner->AddComponent<scene::LineRendererComponent>(); m_owner->AddComponent<scene::SpriteRendererComponent>();
        m_owner->AddComponent<scene::SortingGroupComponent>(); m_owner->AddComponent<scene::TrailComponent>();
        m_owner->AddComponent<scene::MeshTrailComponent>(); m_owner->AddComponent<scene::MeshRenderer>();
        m_owner->AddComponent<scene::SkinnedMeshRenderer>();
        auto& entry = m_owner->AddComponent<scene::ScriptComponent>().scripts.emplace_back();
        entry.script = scene::ScriptFactory::Create("cs-internal:ManagedFixtures.RenderComponentsComponent", entry.savedScriptId);
        ASSERT_NE(entry.script, nullptr); entry.script->SetContext(&m_scene, m_owner);
        m_adapter = runtime.FindAdapter(entry.script.get()); ASSERT_NE(m_adapter, nullptr); ASSERT_TRUE(m_adapter->RefreshOwnerBinding());
    }
    void TearDown() override
    {
        auto& runtime = scene::ManagedScriptRuntime::Instance(); runtime.Unload(&m_scene); m_scene.Clear();
        m_owner = nullptr; m_adapter = nullptr; EXPECT_TRUE(runtime.Shutdown()) << runtime.LastDiagnostic();
        scene::Script::SetInPlayMode(m_previousPlay); EngineFixture::TearDown();
    }
    bool Call(int operation)
    {
        if (!m_adapter->ApplySettingsJson("{\"operation\":" + std::to_string(operation) + "}")) return false;
        return m_adapter->ExecuteProfiledCallback(&scene::Script::OnUpdate, scene::ScriptCallbackKind::UPDATE, "OnUpdate");
    }
    scene::ManagedBridgeReply Field(std::string_view name)
    {
        scene::ManagedBridgeRequest request; request.op = scene::ManagedBridgeOp::READ_FIELD;
        request.generation = m_adapter->Generation(); request.instanceId = m_adapter->InstanceId(); request.ownerId = m_adapter->OwnerId();
        request.input = name.data(); request.inputBytes = static_cast<uint32_t>(name.size());
        scene::ManagedBridgeReply reply; EXPECT_TRUE(scene::ManagedScriptRuntime::Instance().Dispatch(request, reply)); return reply;
    }
};

TEST_F(ManagedRenderComponentsRuntimeTest, ActualClrFacadesReachDistinctNativeRowsAndReturnQueries)
{
    auto& trail = *m_owner->GetComponent<scene::TrailComponent>(); trail.ringCount = 3;
    ASSERT_TRUE(Call(1)) << m_adapter->FaultDiagnostic();
    EXPECT_EQ(Field("connectionMask").integer, 127); EXPECT_EQ(Field("observedCount").integer, 2);
    EXPECT_FLOAT_EQ(Field("observedWidth").x, 0.8f); EXPECT_NE(Field("capacityPreserved").integer, 0);
    const auto observed = Field("observedPosition"); EXPECT_VEC3_NEAR((math::Vector3{observed.x, observed.y, observed.z}), (math::Vector3{8, 9, 10}), 0.00001f);
    const auto* line = m_owner->GetComponent<scene::LineRendererComponent>(); ASSERT_NE(line, nullptr);
    EXPECT_EQ(line->shape, scene::LineShape::Tube); EXPECT_EQ(line->space, scene::LineSpace::World);
    EXPECT_FALSE(line->billboard); EXPECT_TRUE(line->loop); EXPECT_EQ(line->radialSegments, 7);
    EXPECT_EQ(line->sortingLayer, INT32_MIN); EXPECT_EQ(line->orderInLayer, INT32_MAX); EXPECT_FLOAT_EQ(line->endColor.w, 0.125f);
    const auto* sprite = m_owner->GetComponent<scene::SpriteRendererComponent>(); ASSERT_NE(sprite, nullptr);
    EXPECT_TRUE(sprite->receiveLighting); EXPECT_TRUE(sprite->flipX); EXPECT_EQ(sprite->drawMode, scene::SpriteDrawMode::Sliced);
    EXPECT_FALSE(trail.enabled); EXPECT_FALSE(trail.clearOnDisable); EXPECT_EQ(trail.ringCount, 3); EXPECT_EQ(trail.attachBone, "Hand");
    EXPECT_FLOAT_EQ(trail.uvScrollSpeed, -2); EXPECT_FLOAT_EQ(trail.uvTiling, 3);
    EXPECT_FALSE(m_owner->GetComponent<scene::MeshRenderer>()->enabled); EXPECT_FALSE(m_owner->GetComponent<scene::MeshRenderer>()->castShadows);
    EXPECT_FALSE(m_owner->GetComponent<scene::SkinnedMeshRenderer>()->enabled);
    EXPECT_FALSE(m_adapter->IsRuntimeFaulted());
}

TEST_F(ManagedRenderComponentsRuntimeTest, RemovedFacadeAndSavedReferenceNeverAttachToNewBirth)
{
    ASSERT_TRUE(Call(1)); ASSERT_TRUE(Call(3)); EXPECT_EQ(Field("cachedIsAlive").integer, 0); EXPECT_NE(Field("savedReferenceMissing").integer, 0);
    RenderControls::FlushScriptRemovals(m_scene); ASSERT_EQ(m_owner->GetComponent<scene::LineRendererComponent>(), nullptr);
    ASSERT_TRUE(Call(4)) << m_adapter->FaultDiagnostic();
    EXPECT_EQ(Field("cachedIsAlive").integer, 0); EXPECT_NE(Field("savedReferenceMissing").integer, 0); EXPECT_NE(Field("staleWriteRejected").integer, 0);
    const auto* replacement = m_owner->GetComponent<scene::LineRendererComponent>(); ASSERT_NE(replacement, nullptr);
    EXPECT_FLOAT_EQ(replacement->startWidth, 0.05f); EXPECT_FLOAT_EQ(replacement->endWidth, 0.05f);
}

TEST_F(ManagedRenderComponentsRuntimeTest, TypedFacadeOnAnotherObjectUsesThatObjectsNativeRows)
{
    auto& target = m_scene.CreateGameObject("Remote render target");
    target.AddComponent<scene::LineRendererComponent>(); target.AddComponent<scene::SpriteRendererComponent>();
    target.AddComponent<scene::SortingGroupComponent>(); target.AddComponent<scene::TrailComponent>(); target.AddComponent<scene::MeshTrailComponent>();
    target.AddComponent<scene::MeshRenderer>(); target.AddComponent<scene::SkinnedMeshRenderer>();
    ASSERT_TRUE(m_adapter->ApplySettingsJson("{\"target\":\"" + target.instanceId + "\"}"));
    ASSERT_TRUE(Call(1)) << m_adapter->FaultDiagnostic();
    EXPECT_EQ(target.GetComponent<scene::LineRendererComponent>()->points.size(), 2u);
    EXPECT_TRUE(m_owner->GetComponent<scene::LineRendererComponent>()->points.empty());
    EXPECT_FALSE(target.GetComponent<scene::MeshRenderer>()->enabled);
    EXPECT_TRUE(m_owner->GetComponent<scene::MeshRenderer>()->enabled);
}

TEST_F(ManagedRenderComponentsRuntimeTest, InvalidWritesAndEmptySharedMaterialAreValidatedThroughRealClr)
{
    ASSERT_TRUE(Call(1)); ASSERT_TRUE(Call(5)) << m_adapter->FaultDiagnostic();
    EXPECT_EQ(Field("invalidRejected").integer, 7); EXPECT_FLOAT_EQ(Field("observedWidth").x, 0.3f);
    ASSERT_TRUE(Call(7)); EXPECT_NE(Field("materialCleared").integer, 0);
    EXPECT_TRUE(m_owner->GetComponent<scene::LineRendererComponent>()->materialPath.empty());
    EXPECT_TRUE(m_owner->GetComponent<scene::SpriteRendererComponent>()->materialPath.empty());
    EXPECT_TRUE(m_owner->GetComponent<scene::TrailComponent>()->materialPath.empty());
    EXPECT_TRUE(m_owner->GetComponent<scene::MeshTrailComponent>()->materialPath.empty());
}

TEST_F(ManagedRenderComponentsRuntimeTest, MeshClearFromClrAlsoClearsStaticPrimitiveReference)
{
    auto* mesh = m_owner->GetComponent<scene::MeshRenderer>(); ASSERT_NE(mesh, nullptr);
    mesh->meshPath = "primitive:sphere"; mesh->meshPathDirty = true;
    ASSERT_TRUE(Call(6)) << m_adapter->FaultDiagnostic();
    EXPECT_TRUE(mesh->meshPath.empty()); EXPECT_EQ(mesh->mesh, nullptr); EXPECT_TRUE(mesh->enabled);
}

TEST_F(ManagedRenderComponentsRuntimeTest, ActualFreshClrMeshSupportsPrimitiveAndTypedEmptyModelAssignment)
{
    ASSERT_TRUE(m_scene.GetEntities<scene::ProceduralMeshComponent>().empty());
    ASSERT_TRUE(Call(8)) << m_adapter->FaultDiagnostic();
    auto* primitive = m_scene.Find("FreshClrPrimitive");
    ASSERT_NE(primitive, nullptr);
    auto* mesh = primitive->GetComponent<scene::MeshRenderer>();
    ASSERT_NE(mesh, nullptr);
    EXPECT_EQ(mesh->meshPath, "primitive:cube");
    EXPECT_TRUE(mesh->meshPathDirty);
    EXPECT_FALSE(mesh->castShadows);
    EXPECT_TRUE(mesh->enabled);
    EXPECT_EQ(primitive->GetComponent<scene::ProceduralMeshComponent>(), nullptr);
    ASSERT_TRUE(Call(9)) << m_adapter->FaultDiagnostic();
    auto* cleared = m_scene.Find("FreshClrEmptyModel");
    ASSERT_NE(cleared, nullptr);
    auto* clearedMesh = cleared->GetComponent<scene::MeshRenderer>();
    ASSERT_NE(clearedMesh, nullptr);
    EXPECT_NE(Field("modelAccepted").integer, 0);
    EXPECT_TRUE(clearedMesh->meshPath.empty());
    EXPECT_TRUE(clearedMesh->enabled);
    EXPECT_EQ(cleared->GetComponent<scene::ProceduralMeshComponent>(), nullptr);
    EXPECT_EQ(mesh->meshPath, "primitive:cube");
}

TEST_F(ManagedRenderComponentsRuntimeTest, CatalogAndSettingsSnapshotPreserveRegisteredRenderReferenceIdentity)
{
    std::string catalog; ASSERT_TRUE(scene::ManagedScriptRuntime::Instance().GetApiCatalog(catalog));
    for (const char* type : {"Fbzz.LineRenderer", "Fbzz.SpriteRenderer", "Fbzz.SortingGroup", "Fbzz.TrailRenderer", "Fbzz.MeshTrail", "Fbzz.MeshRenderer", "Fbzz.SkinnedMeshRenderer"})
        EXPECT_NE(catalog.find(type), std::string::npos) << type;
    ASSERT_TRUE(Call(1)); std::string snapshot; ASSERT_TRUE(m_adapter->ReadSettingsSnapshot(snapshot));
    EXPECT_NE(snapshot.find("lineReference"), std::string::npos);
    EXPECT_NE(snapshot.find(m_owner->instanceId), std::string::npos);
    EXPECT_NE(snapshot.find("\"componentKind\":35"), std::string::npos);
    const auto source = scene::SceneSerializer::SaveToText(m_scene); ASSERT_FALSE(source.empty());
    auto restored = scene::SceneSerializer::LoadDataFromText(source, "render-components.fbzz"); ASSERT_NE(restored, nullptr);
    auto* restoredOwner = restored->Find("CLR Render"); ASSERT_NE(restoredOwner, nullptr);
    auto* scripts = restoredOwner->GetComponent<scene::ScriptComponent>(); ASSERT_NE(scripts, nullptr); ASSERT_EQ(scripts->scripts.size(), 1u);
    auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(scripts->scripts[0].script.get()); ASSERT_NE(adapter, nullptr);
    ASSERT_TRUE(adapter->ApplySettingsJson("{\"operation\":2}"));
    ASSERT_TRUE(adapter->ExecuteProfiledCallback(&scene::Script::OnUpdate, scene::ScriptCallbackKind::UPDATE, "OnUpdate"));
    ASSERT_TRUE(adapter->ReadSettingsSnapshot(snapshot));
    EXPECT_NE(snapshot.find("\"savedReferenceMissing\":false"), std::string::npos);
    EXPECT_NE(snapshot.find("\"componentKind\":35"), std::string::npos);
    EXPECT_EQ(restoredOwner->GetComponent<scene::LineRendererComponent>()->points.size(), 2u);
}
} /// @note namespace fbzz::tests
