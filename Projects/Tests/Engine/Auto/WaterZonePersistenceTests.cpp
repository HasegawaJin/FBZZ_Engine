/// @file    WaterZonePersistenceTests.cpp
/// @brief   Zoneの参照保存・Play複製・シーン追記と単独浮力の契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/TargetPointComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Components/WaterZoneComponent.hpp>
#include <Engine/Scene/ControlPointAdapters.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/WaterSurfaceQuery.hpp>
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include <Engine/Scene/Systems/PhysicsSystem.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Physics/RigidBody.hpp>
#include <Physics/World.hpp>
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {
class WaterZonePersistenceTest : public testkit::EngineFixture {};
class WaterZonePhysicsTest : public testkit::EngineFixture {};

math::Vector3 StepOverlappingWaters(bool addMember, bool connectZone)
{
    scene::Scene source;
    physics::World world;
    auto& owner = source.CreateGameObject("Owner");
    auto& water = owner.AddComponent<scene::WaterComponent>();
    water.enableGerstnerWaves = false;
    water.extentX = water.extentZ = 10.0f;
    water.buoyancyDepth = 5.0f;
    water.buoyancy = 20.0f;
    water.waterDrag = 0.0f;
    if (addMember) {
        auto& member = source.CreateGameObject("Member");
        member.AddComponent<scene::WaterComponent>(water);
        if (connectZone) {
            auto& zone = owner.AddComponent<scene::WaterZoneComponent>();
            zone.members.push_back({scene::EntityRef{member.GetID()}, 0});
        }
    }
    auto& object = source.CreateGameObject("Floating Body");
    object.transform.position = {0.0f, -2.0f, 0.0f};
    scene::FlushWorldTransforms(source);
    auto& component = object.AddComponent<scene::RigidBodyComponent>();
    component.rigidBody = std::make_unique<physics::RigidBody>();
    component.rigidBody->SetPosition(object.transform.worldPosition);
    component.rigidBody->m_useGravity = false;
    component.rigidBody->m_allowSleeping = false;
    SystemContext context{source, world, nullptr, nullptr, 0.01f, 0.01f, true, true};
    scene::PhysicsSystem system;
    system.Update(context);
    return component.rigidBody->GetVelocity();
}

void CreateConnectedWaters(scene::Scene& source)
{
    auto& owner = source.CreateGameObject("Zone Parent");
    auto& parentWater = owner.AddComponent<scene::WaterComponent>();
    parentWater.extentX = parentWater.extentZ = 10.0f;
    parentWater.enableGerstnerWaves = false;
    auto& riverObject = source.CreateGameObject("River Member");
    auto& river = riverObject.AddComponent<scene::WaterComponent>();
    river.enableGerstnerWaves = false;
    river.surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
    river.splineSamplesPerSegment = 2;
    river.splinePoints = {
        {1, {-8.0f, 1.0f, 0.0f}, 2.0f, 2.0f, 1.0f},
        {2, {-4.0f, 0.5f, 0.0f}, 2.0f, 3.0f, 2.0f},
        {3, { 0.0f, 0.0f, 0.0f}, 2.0f, 4.0f, 3.0f}
    };
    auto& zone = owner.AddComponent<scene::WaterZoneComponent>();
    zone.ownerPriority = -1;
    zone.meshCellSize = 1.0f;
    zone.members.push_back({scene::EntityRef{riverObject.GetID()}, 5});
    zone.riverMouths.push_back({scene::EntityRef{riverObject.GetID()}, false, 4.0f});
}
} /// @note namespace

TEST_F(WaterZonePersistenceTest, HorizonLinkSurvivesReloadAndSharesTheSimulatedMouthHeight)
{
    scene::Scene source;
    auto& oceanObject = source.CreateGameObject("Horizontal Sea");
    oceanObject.AddComponent<scene::WaterComponent>().farOceanEnabled = true;
    auto& riverObject = source.CreateGameObject("River");
    auto& river = riverObject.AddComponent<scene::WaterComponent>();
    river.surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
    river.splinePoints = {
        {1, {-32.0f, 2.0f, 0.0f}, 8.0f, 4.0f, 1.0f},
        {2, {-16.0f, 1.0f, 0.0f}, 8.0f, 4.0f, 1.0f},
        {3, {  0.0f, 0.25f, 0.0f}, 8.0f, 4.0f, 1.0f},
    };
    river.horizonMaterialReceiver = scene::EntityRef{oceanObject.GetID()};
    river.horizonMaterialBlendLength = 12.0f;

    auto restored = scene::SceneSerializer::LoadDataFromText(scene::SceneSerializer::SaveToText(source), "");
    ASSERT_NE(restored, nullptr);
    auto* restoredRiver = restored->Find("River");
    auto* restoredOcean = restored->Find("Horizontal Sea");
    ASSERT_NE(restoredRiver, nullptr);
    ASSERT_NE(restoredOcean, nullptr);
    auto* riverWater = restoredRiver->GetComponent<scene::WaterComponent>();
    const auto* oceanWater = restoredOcean->GetComponent<scene::WaterComponent>();
    ASSERT_EQ(riverWater->horizonMaterialReceiver.Resolve(*restored), restoredOcean);
    EXPECT_FLOAT_EQ(riverWater->horizonMaterialBlendLength, 12.0f);
    scene::FlushWorldTransforms(*restored);
    scene::EnsureWaterSimulationSurfaces(*restored);
    for (const float time : {0.0f, 1.5f, 3.0f}) {
        scene::WaterSurfaceHeightSample riverSample, oceanSample;
        const math::Vector3 point{0.0f, -0.5f, 3.0f};
        const auto view = scene::WaterSurfaceReadView::SIMULATION;
        ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(*riverWater, restoredRiver->transform,
            point, time, riverSample, view));
        ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(*oceanWater, restoredOcean->transform,
            point, time, oceanSample, view));
        EXPECT_NEAR(riverSample.position.y, oceanSample.position.y, testkit::kTolerance);
        EXPECT_TRUE(riverSample.underwater);
        EXPECT_TRUE(oceanSample.underwater);
    }
}

TEST_F(WaterZonePersistenceTest, PassiveCloneRestoresNestedReferencesAndOmitsRuntimeState)
{
    scene::Scene source;
    CreateConnectedWaters(source);
    scene::PrepareWaterSceneSurfaces(source);
    auto* originalOwner = source.Find("Zone Parent");
    ASSERT_NE(originalOwner, nullptr);
    ASSERT_NE(originalOwner->GetComponent<scene::WaterZoneComponent>()->snapshot, nullptr);
    std::string diagnostic;
    const auto text = scene::SceneSerializer::CapturePassiveClone(source, diagnostic);
    ASSERT_FALSE(text.empty()) << diagnostic;
    EXPECT_EQ(text.find("zoneSnapshot"), std::string::npos);
    EXPECT_EQ(text.find("attemptSignature"), std::string::npos);
    EXPECT_EQ(text.find("zoneOwner"), std::string::npos);
    auto restored = scene::SceneSerializer::LoadDataFromText(text, "");
    ASSERT_NE(restored, nullptr);
    auto* owner = restored->Find("Zone Parent");
    auto* river = restored->Find("River Member");
    ASSERT_NE(owner, nullptr);
    ASSERT_NE(river, nullptr);
    auto* zone = owner->GetComponent<scene::WaterZoneComponent>();
    ASSERT_NE(zone, nullptr);
    ASSERT_EQ(zone->members.size(), 1u);
    ASSERT_EQ(zone->riverMouths.size(), 1u);
    EXPECT_EQ(zone->members[0].water.id, river->GetID());
    EXPECT_EQ(zone->members[0].priority, 5);
    EXPECT_EQ(zone->riverMouths[0].river.id, river->GetID());
    EXPECT_FALSE(zone->riverMouths[0].atStart);
    EXPECT_FLOAT_EQ(zone->riverMouths[0].blendLength, 4.0f);
    EXPECT_FLOAT_EQ(zone->meshCellSize, 1.0f);
    EXPECT_EQ(zone->ownerPriority, -1);
    EXPECT_FALSE(zone->snapshot);
    EXPECT_FALSE(zone->attempted);
    scene::PrepareWaterSceneSurfaces(*restored);
    ASSERT_NE(zone->snapshot, nullptr) << zone->diagnostic;
    EXPECT_NE(zone->snapshot, originalOwner->GetComponent<scene::WaterZoneComponent>()->snapshot);
}

TEST_F(WaterZonePersistenceTest, AppendingWatersResolvesZoneReferencesToNewObjects)
{
    scene::Scene source;
    CreateConnectedWaters(source);
    const auto text = scene::SceneSerializer::SaveToText(source);
    ASSERT_FALSE(text.empty());
    scene::Scene destination;
    auto& unrelated = destination.CreateGameObject("Unrelated");
    const auto unrelatedID = unrelated.GetID();
    std::vector<scene::EntityID> roots;
    ASSERT_TRUE(scene::SceneSerializer::AppendObjects(destination, text, nullptr, roots));
    auto* owner = destination.Find("Zone Parent");
    auto* river = destination.Find("River Member");
    ASSERT_NE(owner, nullptr);
    ASSERT_NE(river, nullptr);
    const auto* ownerWater = owner->GetComponent<scene::WaterComponent>();
    const auto* riverWater = river->GetComponent<scene::WaterComponent>();
    ASSERT_NE(ownerWater, nullptr);
    ASSERT_NE(riverWater, nullptr);
    EXPECT_FLOAT_EQ(ownerWater->extentX, 10.0f);
    EXPECT_FLOAT_EQ(ownerWater->extentZ, 10.0f);
    EXPECT_EQ(riverWater->surfaceShape, scene::WaterSurfaceShape::SPLINE_RIVER);
    ASSERT_EQ(riverWater->splinePoints.size(), 3u);
    EXPECT_FLOAT_EQ(riverWater->splinePoints[0].localPosition.x, -8.0f);
    EXPECT_FLOAT_EQ(riverWater->splinePoints[0].localPosition.y, 1.0f);
    auto* zone = owner->GetComponent<scene::WaterZoneComponent>();
    ASSERT_NE(zone, nullptr);
    ASSERT_EQ(zone->members.size(), 1u);
    ASSERT_EQ(zone->riverMouths.size(), 1u);
    EXPECT_EQ(zone->members[0].water.id, river->GetID());
    EXPECT_EQ(zone->riverMouths[0].river.id, river->GetID());
    EXPECT_NE(zone->members[0].water.id, unrelatedID);
    scene::PrepareWaterSceneSurfaces(destination);
    EXPECT_NE(zone->snapshot, nullptr) << zone->diagnostic;
    EXPECT_EQ(destination.GetGameObject(unrelatedID), &unrelated);
}

TEST_F(WaterZonePersistenceTest, AppendingPointObjectWaterPreservesAndValidatesStaticBake)
{
    scene::Scene source;
    auto& sourceOwner = source.CreateGameObject("Baked Lake");
    const scene::EntityID ownerId = sourceOwner.GetID();
    const std::string ownerGuid = sourceOwner.instanceId;
    auto& authoredWater = sourceOwner.AddComponent<scene::WaterComponent>();
    authoredWater.surfaceShape = scene::WaterSurfaceShape::SPLINE_LAKE;
    authoredWater.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    authoredWater.pointRuntimeMode = scene::ControlPointRuntimeMode::BAKED_STATIC;
    authoredWater.lakePlaneHeight = 1.25f;

    struct MarkerIdentity {
        std::string guid;
    };
    const std::array<math::Vector3, 4> positions = {{
        {-4.0f, 1.25f, -3.0f}, {4.0f, 1.25f, -3.0f},
        {4.0f, 1.25f, 3.0f}, {-4.0f, 1.25f, 3.0f}
    }};
    const std::array<float, 4> widths = {1.25f, 1.5f, 1.75f, 2.0f};
    const std::array<float, 4> depths = {2.5f, 3.0f, 3.5f, 4.0f};
    const std::array<float, 4> flows = {0.5f, 1.0f, 1.5f, 2.0f};
    std::vector<MarkerIdentity> markerIdentities;
    std::vector<scene::WaterControlPointBinding> bindings;
    markerIdentities.reserve(positions.size());
    bindings.reserve(positions.size());
    for (std::size_t index = 0; index < positions.size(); ++index) {
        auto& marker = source.CreateGameObject("Lake Point " + std::to_string(index));
        marker.SetParent(*source.GetGameObject(ownerId));
        marker.transform.position = positions[index];
        marker.AddComponent<scene::TargetPointComponent>();
        markerIdentities.push_back({marker.instanceId});
        bindings.push_back({
            static_cast<std::uint32_t>(101 + index),
            scene::ObjectReference{marker.instanceId},
            widths[index], depths[index], flows[index]});
    }
    auto* sourceWater = source.GetGameObject(ownerId)->GetComponent<scene::WaterComponent>();
    ASSERT_NE(sourceWater, nullptr);
    sourceWater->pointBindings = std::move(bindings);
    scene::FlushWorldTransforms(source);

    auto authored = scene::ResolveWaterControlPointAuthoring(
        source, ownerId, *sourceWater, scene::ControlPointRuntimeMode::BAKED_STATIC);
    ASSERT_TRUE(authored.valid);
    ASSERT_EQ(authored.points.size(), positions.size());
    sourceWater->bakedSplinePoints = authored.points;
    sourceWater->bakedInputSignature = authored.inputSignature;
    sourceWater->bakedSourceMode = sourceWater->pointSourceMode;
    sourceWater->bakedSurfaceShape = sourceWater->surfaceShape;
    sourceWater->bakedLakePlaneHeight = sourceWater->lakePlaneHeight;
    sourceWater->bakedAdapterVersion = scene::CONTROL_POINT_ADAPTER_VERSION;
    sourceWater->bakeValid = true;
    const std::uint64_t bakeSignature = sourceWater->bakedInputSignature;
    ASSERT_NE(bakeSignature, 0u);

    const std::string text = scene::SceneSerializer::SaveToText(source);
    ASSERT_FALSE(text.empty());
    scene::Scene destination;
    auto& unrelated = destination.CreateGameObject("Unrelated");
    const scene::EntityID unrelatedId = unrelated.GetID();
    const std::string unrelatedGuid = unrelated.instanceId;
    const std::size_t originalObjectCount = destination.GameObjectCount();
    std::vector<scene::EntityID> roots;
    ASSERT_TRUE(scene::SceneSerializer::AppendObjects(destination, text, nullptr, roots));

    auto* appendedOwner = destination.FindByGuid(ownerGuid);
    ASSERT_NE(appendedOwner, nullptr);
    auto* appendedWater = appendedOwner->GetComponent<scene::WaterComponent>();
    ASSERT_NE(appendedWater, nullptr);
    EXPECT_EQ(destination.GameObjectCount(), originalObjectCount + 1 + positions.size());
    auto* preservedUnrelated = destination.GetGameObject(unrelatedId);
    ASSERT_NE(preservedUnrelated, nullptr);
    EXPECT_EQ(preservedUnrelated->instanceId, unrelatedGuid);
    EXPECT_EQ(destination.FindByGuid(unrelatedGuid), preservedUnrelated);
    EXPECT_EQ(appendedWater->pointSourceMode, scene::ControlPointSourceMode::POINT_OBJECTS);
    EXPECT_EQ(appendedWater->pointRuntimeMode, scene::ControlPointRuntimeMode::BAKED_STATIC);
    EXPECT_EQ(appendedWater->bakedSourceMode, scene::ControlPointSourceMode::POINT_OBJECTS);
    EXPECT_EQ(appendedWater->surfaceShape, scene::WaterSurfaceShape::SPLINE_LAKE);
    EXPECT_EQ(appendedWater->bakedSurfaceShape, scene::WaterSurfaceShape::SPLINE_LAKE);
    EXPECT_FLOAT_EQ(appendedWater->lakePlaneHeight, 1.25f);
    EXPECT_FLOAT_EQ(appendedWater->bakedLakePlaneHeight, 1.25f);
    EXPECT_EQ(appendedWater->bakedAdapterVersion, scene::CONTROL_POINT_ADAPTER_VERSION);
    EXPECT_EQ(appendedWater->bakedInputSignature, bakeSignature);
    EXPECT_TRUE(appendedWater->bakeValid);
    EXPECT_TRUE(appendedWater->splinePoints.empty());
    EXPECT_FALSE(appendedWater->splineSnapshot);
    EXPECT_FALSE(appendedWater->zoneSnapshot);
    EXPECT_FALSE(appendedWater->depthField);
    EXPECT_FALSE(appendedWater->simulationZoneSnapshot);
    EXPECT_FALSE(appendedWater->simulationSplineSnapshot);
    EXPECT_FALSE(appendedWater->simulationDepthField);
    EXPECT_FALSE(appendedWater->simulationSurfacePublished);
    ASSERT_EQ(appendedWater->pointBindings.size(), markerIdentities.size());
    ASSERT_EQ(appendedWater->bakedSplinePoints.size(), authored.points.size());
    for (std::size_t index = 0; index < markerIdentities.size(); ++index) {
        EXPECT_EQ(appendedWater->pointBindings[index].id, 101u + static_cast<std::uint32_t>(index));
        EXPECT_EQ(appendedWater->pointBindings[index].target.guid, markerIdentities[index].guid);
        EXPECT_FLOAT_EQ(appendedWater->pointBindings[index].width, widths[index]);
        EXPECT_FLOAT_EQ(appendedWater->pointBindings[index].depth, depths[index]);
        EXPECT_FLOAT_EQ(appendedWater->pointBindings[index].flowSpeed, flows[index]);
        EXPECT_EQ(appendedWater->bakedSplinePoints[index], authored.points[index]);
        auto* appendedMarker = destination.FindByGuid(markerIdentities[index].guid);
        ASSERT_NE(appendedMarker, nullptr);
        ASSERT_NE(appendedMarker->GetComponent<scene::TargetPointComponent>(), nullptr);
        EXPECT_EQ(appendedMarker->GetParent(), appendedOwner);
        EXPECT_EQ(appendedWater->pointBindings[index].target.id, appendedMarker->GetID());
        EXPECT_EQ(appendedWater->pointBindings[index].target.sceneGeneration,
            destination.GetRenderSceneGeneration());
        EXPECT_NE(appendedWater->pointBindings[index].target.id, unrelatedId);
    }

    auto runtimeBake = scene::ResolveWaterControlPoints(
        destination, appendedOwner->GetID(), *appendedWater,
        scene::ControlPointReadContext::RUNTIME);
    ASSERT_TRUE(runtimeBake.valid) << static_cast<unsigned>(runtimeBake.status);
    EXPECT_EQ(runtimeBake.points, authored.points);
    EXPECT_EQ(runtimeBake.surfaceShape, scene::WaterSurfaceShape::SPLINE_LAKE);
    EXPECT_FLOAT_EQ(runtimeBake.lakePlaneHeight, 1.25f);

    auto resolved = scene::ResolveWaterControlPoints(
        destination, appendedOwner->GetID(), *appendedWater,
        scene::ControlPointReadContext::AUTHORING);
    ASSERT_TRUE(resolved.valid) << static_cast<unsigned>(resolved.status);
    EXPECT_FALSE(resolved.bakeStale);
    EXPECT_EQ(resolved.status, scene::ControlPointAdapterStatus::SUCCESS);
    EXPECT_EQ(resolved.points, authored.points);
    for (std::size_t index = 0; index < markerIdentities.size(); ++index) {
        auto* appendedMarker = destination.FindByGuid(markerIdentities[index].guid);
        ASSERT_NE(appendedMarker, nullptr);
        EXPECT_EQ(appendedWater->pointBindings[index].target.id, appendedMarker->GetID());
        EXPECT_EQ(appendedWater->pointBindings[index].target.sceneGeneration,
            destination.GetRenderSceneGeneration());
    }
    EXPECT_EQ(preservedUnrelated->name, "Unrelated");
}

TEST_F(WaterZonePersistenceTest, OversizedPersistedMembersCannotBecomeAnOwnerOnlyZone)
{
    scene::Scene source;
    CreateConnectedWaters(source);
    auto* owner = source.Find("Zone Parent");
    auto* zone = owner->GetComponent<scene::WaterZoneComponent>();
    zone->members.resize(scene::WATER_ZONE_MAX_BODIES);
    const auto text = scene::SceneSerializer::SaveToText(source);
    ASSERT_FALSE(text.empty());
    auto restored = scene::SceneSerializer::LoadDataFromText(text, "");
    ASSERT_NE(restored, nullptr);
    auto* restoredZone = restored->Find("Zone Parent")->GetComponent<scene::WaterZoneComponent>();
    ASSERT_NE(restoredZone, nullptr);
    EXPECT_GT(restoredZone->members.size(), scene::WATER_ZONE_MAX_BODIES - 1);
    scene::PrepareWaterSceneSurfaces(*restored);
    EXPECT_FALSE(restoredZone->snapshot);
    EXPECT_FALSE(restoredZone->diagnostic.empty());
}

TEST_F(WaterZonePhysicsTest, ConnectedOverlapsApplyTheSameLiftAsOneWater)
{
    const auto oneWater = StepOverlappingWaters(false, false);
    const auto independentOverlap = StepOverlappingWaters(true, false);
    const auto connectedOverlap = StepOverlappingWaters(true, true);
    EXPECT_GT(oneWater.y, 0.0f);
    EXPECT_GT(independentOverlap.y, oneWater.y * 1.5f);
    EXPECT_VEC3_NEAR(connectedOverlap, oneWater, testkit::kTolerance);
}
} /// @note namespace fbzz::tests
