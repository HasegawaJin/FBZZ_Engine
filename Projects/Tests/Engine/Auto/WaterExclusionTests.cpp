/// @file    WaterExclusionTests.cpp
/// @brief   Waterの3D除外箱・共有snapshot・水面と浮力の所属を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-09
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Components/WaterExclusionComponent.hpp>
#include <Engine/Scene/Components/WaterZoneComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include <Engine/Scene/WaterExclusion.hpp>
#include <Engine/Scene/WaterInteraction.hpp>
#include <Engine/Scene/WaterSurfaceQuery.hpp>
#include <Physics/FluidVolume.hpp>
#include <Physics/RigidBody.hpp>
#include <memory>
#include <string>
#include <utility>

namespace fbzz::tests {
namespace {

class WaterExclusionTest : public testkit::EngineFixture {};

void ConfigureWater(scene::WaterComponent& water, float extent = 10.0f)
{
    water.extentX = extent;
    water.extentZ = extent;
    water.buoyancyDepth = 5.0f;
    water.enableGerstnerWaves = false;
}

} /// @note namespace

TEST_F(WaterExclusionTest, OrientedBoxUsesFullWorldRotationAndSignedNonUniformScale)
{
    scene::Scene scene;
    auto& parent = scene.CreateGameObject("Exclusion Parent");
    parent.transform.position = {7.0f, 2.0f, -4.0f};
    parent.transform.rotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(37.0f));
    parent.transform.scale = {-2.0f, 3.0f, 0.5f};
    auto& boxObject = scene.CreateGameObject("Exclusion Box");
    boxObject.transform.position = {1.0f, -0.5f, 2.0f};
    boxObject.transform.rotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(-21.0f));
    boxObject.transform.scale = {1.5f, 0.75f, 2.0f};
    boxObject.SetParent(parent);
    auto& exclusion = boxObject.AddComponent<scene::WaterExclusionComponent>();
    exclusion.excludeAll = true;
    exclusion.size = {2.0f, 4.0f, 6.0f};

    auto& waterObject = scene.CreateGameObject("Water");
    auto& water = waterObject.AddComponent<scene::WaterComponent>();
    ConfigureWater(water);
    scene::FlushWorldTransforms(scene);
    scene::PrepareWaterSceneSurfaces(scene);

    ASSERT_NE(water.exclusionSnapshot, nullptr);
    ASSERT_EQ(water.exclusionSnapshot->boxes.size(), 1u);
    const scene::WaterExclusionBox& box = water.exclusionSnapshot->boxes.front();
    const math::Vector3 localPoint{0.5f, -0.75f, 1.25f};
    const math::Vector3 scaledPoint{
        localPoint.x * boxObject.transform.worldScale.x,
        localPoint.y * boxObject.transform.worldScale.y,
        localPoint.z * boxObject.transform.worldScale.z
    };
    const math::Vector3 worldPoint = boxObject.transform.worldPosition
        + boxObject.transform.worldRotation * scaledPoint;
    EXPECT_TRUE(box.Contains(worldPoint));

    const math::Vector3 outsideLocal{1.1f, 0.0f, 0.0f};
    const math::Vector3 outsideScaled{
        outsideLocal.x * boxObject.transform.worldScale.x,
        outsideLocal.y * boxObject.transform.worldScale.y,
        outsideLocal.z * boxObject.transform.worldScale.z
    };
    const math::Vector3 outsideWorld = boxObject.transform.worldPosition
        + boxObject.transform.worldRotation * outsideScaled;
    EXPECT_FALSE(box.Contains(outsideWorld));
}

TEST_F(WaterExclusionTest, SurfaceCutPointAndBelowBoxUnderwaterMembershipRemainDistinct)
{
    scene::Scene scene;
    auto& waterObject = scene.CreateGameObject("Water");
    auto& water = waterObject.AddComponent<scene::WaterComponent>();
    ConfigureWater(water);
    auto& boxObject = scene.CreateGameObject("Cave Exclusion");
    auto& exclusion = boxObject.AddComponent<scene::WaterExclusionComponent>();
    exclusion.size = {2.0f, 2.0f, 2.0f};
    scene::FlushWorldTransforms(scene);
    scene::PrepareWaterSceneSurfaces(scene);

    ASSERT_NE(water.exclusionSnapshot, nullptr);
    EXPECT_TRUE(scene::WaterSurfaceContainsXZ(water, waterObject.transform, 0.0f, 0.0f, 0.0f));
    EXPECT_TRUE(scene::WaterSurfaceContainsXZ(water, waterObject.transform, 3.0f, 0.0f, 0.0f));

    scene::WaterSurfaceHeightSample atSurfaceCut;
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, waterObject.transform,
        {0.0f, 0.0f, 0.0f}, 0.0f, atSurfaceCut));
    EXPECT_TRUE(atSurfaceCut.excluded);
    EXPECT_TRUE(atSurfaceCut.insideSurface);
    EXPECT_FALSE(atSurfaceCut.underwater);

    scene::WaterSurfaceHeightSample belowBox;
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, waterObject.transform,
        {0.0f, -5.0f, 0.0f}, 0.0f, belowBox));
    EXPECT_FALSE(belowBox.excluded);
    EXPECT_TRUE(belowBox.insideSurface);
    EXPECT_TRUE(belowBox.underwater);

    scene::WaterSurfaceHeightSample outsideBox;
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, waterObject.transform,
        {3.0f, -1.0f, 0.0f}, 0.0f, outsideBox));
    EXPECT_FALSE(outsideBox.excluded);
    EXPECT_TRUE(outsideBox.insideSurface);
    EXPECT_TRUE(outsideBox.underwater);
}

TEST_F(WaterExclusionTest, ExplicitZoneMemberReferenceResolvesToSharedOwnerSnapshot)
{
    scene::Scene scene;
    auto& ownerObject = scene.CreateGameObject("Zone Owner");
    auto& ownerWater = ownerObject.AddComponent<scene::WaterComponent>();
    ConfigureWater(ownerWater, 10.0f);
    auto& memberObject = scene.CreateGameObject("Zone Member");
    auto& memberWater = memberObject.AddComponent<scene::WaterComponent>();
    ConfigureWater(memberWater, 4.0f);
    memberObject.transform.position = {10.0f, 0.0f, 0.0f};
    auto& zone = ownerObject.AddComponent<scene::WaterZoneComponent>();
    zone.members.push_back({scene::EntityRef{memberObject.GetID()}, 0});
    auto& boxObject = scene.CreateGameObject("Member Exclusion");
    auto& exclusion = boxObject.AddComponent<scene::WaterExclusionComponent>();
    boxObject.transform.position = {10.0f, 0.0f, 0.0f};
    exclusion.excludeAll = false;
    exclusion.waters.push_back(scene::EntityRef{memberObject.GetID()});
    scene::FlushWorldTransforms(scene);
    scene::PrepareWaterSceneSurfaces(scene);

    ASSERT_NE(zone.snapshot, nullptr) << zone.diagnostic;
    ASSERT_EQ(memberWater.zoneOwner, ownerObject.GetID());
    ASSERT_NE(ownerWater.exclusionSnapshot, nullptr);
    EXPECT_EQ(ownerWater.exclusionSnapshot->boxes.size(), 1u);
    EXPECT_EQ(memberWater.exclusionSnapshot, ownerWater.exclusionSnapshot);
    EXPECT_TRUE(ownerWater.exclusionSnapshot->boxes.front().Contains({10.0f, 0.0f, 0.0f}));
}

TEST_F(WaterExclusionTest, TargetSelectionPersistsAndInactiveObjectReleasesItsSnapshot)
{
    scene::Scene scene;
    auto& firstObject = scene.CreateGameObject("First Water");
    auto& firstWater = firstObject.AddComponent<scene::WaterComponent>();
    ConfigureWater(firstWater);
    auto& secondObject = scene.CreateGameObject("Second Water");
    auto& secondWater = secondObject.AddComponent<scene::WaterComponent>();
    ConfigureWater(secondWater);
    auto& boxObject = scene.CreateGameObject("Targeted Exclusion");
    auto& exclusion = boxObject.AddComponent<scene::WaterExclusionComponent>();
    exclusion.excludeAll = false;
    exclusion.waters.push_back(scene::EntityRef{firstObject.GetID()});
    scene::FlushWorldTransforms(scene);
    scene::PrepareWaterSceneSurfaces(scene);

    ASSERT_NE(firstWater.exclusionSnapshot, nullptr);
    EXPECT_EQ(firstWater.exclusionSnapshot->boxes.size(), 1u);
    EXPECT_EQ(secondWater.exclusionSnapshot, nullptr);
    boxObject.SetActive(false);
    scene::FlushWorldTransforms(scene);
    scene::PrepareWaterSceneSurfaces(scene);
    EXPECT_EQ(firstWater.exclusionSnapshot, nullptr);
}

TEST_F(WaterExclusionTest, SimulationKeepsThePublishedBoxWhileLiveQueryMoves)
{
    scene::Scene scene;
    auto& waterObject = scene.CreateGameObject("Water");
    auto& water = waterObject.AddComponent<scene::WaterComponent>();
    ConfigureWater(water);
    auto& boxObject = scene.CreateGameObject("Moving Exclusion");
    auto& exclusion = boxObject.AddComponent<scene::WaterExclusionComponent>();
    exclusion.size = {2.0f, 2.0f, 2.0f};
    scene::FlushWorldTransforms(scene);
    scene::PrepareWaterSimulationSurfaces(scene);
    const auto simulationSnapshot = water.simulationExclusionSnapshot;
    ASSERT_NE(simulationSnapshot, nullptr);

    boxObject.transform.position = {5.0f, 0.0f, 0.0f};
    scene::FlushWorldTransforms(scene);
    scene::PrepareWaterSceneSurfaces(scene);
    ASSERT_NE(water.exclusionSnapshot, nullptr);
    EXPECT_NE(water.exclusionSnapshot, simulationSnapshot);
    EXPECT_EQ(water.simulationExclusionSnapshot, simulationSnapshot);
    const auto* simulation = scene::WaterSurfaceExclusions(
        water, scene::WaterSurfaceReadView::SIMULATION);
    const auto* live = scene::WaterSurfaceExclusions(
        water, scene::WaterSurfaceReadView::LIVE_QUERY);
    ASSERT_NE(simulation, nullptr);
    ASSERT_NE(live, nullptr);
    EXPECT_TRUE(simulation->boxes.front().Contains({0.0f, 0.0f, 0.0f}));
    EXPECT_FALSE(live->boxes.front().Contains({0.0f, 0.0f, 0.0f}));
}

TEST_F(WaterExclusionTest, BodyCenteredInsideExclusionStillGetsBuoyancyFromOutsideProbes)
{
    scene::Scene scene;
    auto& waterObject = scene.CreateGameObject("Water");
    auto& water = waterObject.AddComponent<scene::WaterComponent>();
    ConfigureWater(water);
    water.buoyancy = 6.0f;
    auto& boxObject = scene.CreateGameObject("Underwater Exclusion");
    boxObject.transform.position = {0.0f, -5.0f, 0.0f};
    auto& exclusion = boxObject.AddComponent<scene::WaterExclusionComponent>();
    exclusion.size = {0.4f, 4.0f, 4.0f};
    scene::FlushWorldTransforms(scene);
    scene::PrepareWaterSimulationSurfaces(scene);

    physics::RigidBody body;
    body.SetMass(2.0f);
    body.SetPosition({0.0f, -5.0f, 0.0f});
    const scene::Transform pose = waterObject.transform;
    const auto snapshot = water.simulationExclusionSnapshot;
    ASSERT_NE(snapshot, nullptr);

    physics::FluidVolumeDesc desc;
    desc.surfaceHeight = [&water](float x, float z, float time) {
        return water.GetSurfaceHeightAt(x, z, time, scene::WaterSurfaceReadView::SIMULATION);
    };
    desc.containsXZ = [&water, &pose](float x, float z, float time) {
        return scene::WaterSurfaceContainsXZ(water, pose, x, z, time,
            scene::WaterSurfaceReadView::SIMULATION);
    };
    desc.pointAllowed = [snapshot](const math::Vector3& position) {
        return !scene::IsWaterPointExcluded(snapshot.get(), position);
    };
    desc.allowProbeIntersection = true;
    desc.center = pose.worldPosition;
    desc.halfX = 5.0f;
    desc.halfZ = 5.0f;
    desc.buoyancy = water.buoyancy;
    desc.drag = 0.0f;
    desc.depthLimit = water.buoyancyDepth;
    desc.surfaceHeightBound = water.SurfaceHeightBound(scene::WaterSurfaceReadView::SIMULATION);
    physics::FluidVolume volume(std::move(desc));

    ASSERT_TRUE(volume.Contains(body.GetPosition()));
    volume.Apply(body, 1.0f);
    body.Integrate(1.0f);

    const math::Vector3 expected{0.0f, 1.5f, 0.0f};
    EXPECT_VEC3_NEAR(body.GetVelocity(), expected, testkit::kTolerance);
}

TEST_F(WaterExclusionTest, AutomaticPersistenceRetainsTypedWaterTargets)
{
    scene::Scene source;
    auto& waterObject = source.CreateGameObject("Target Water");
    waterObject.AddComponent<scene::WaterComponent>();
    auto& boxObject = source.CreateGameObject("Exclusion");
    auto& exclusion = boxObject.AddComponent<scene::WaterExclusionComponent>();
    exclusion.excludeAll = false;
    exclusion.size = {3.0f, 4.0f, 5.0f};
    exclusion.waters.push_back(scene::EntityRef{waterObject.GetID()});
    const std::string serialized = scene::SceneSerializer::SaveToText(source);
    ASSERT_FALSE(serialized.empty());
    auto restored = scene::SceneSerializer::LoadDataFromText(serialized, "");
    ASSERT_NE(restored, nullptr);
    auto* restoredBox = restored->Find("Exclusion");
    ASSERT_NE(restoredBox, nullptr);
    const auto* restoredExclusion = restoredBox->GetComponent<scene::WaterExclusionComponent>();
    ASSERT_NE(restoredExclusion, nullptr);
    EXPECT_FALSE(restoredExclusion->excludeAll);
    EXPECT_EQ(restoredExclusion->waters.size(), 1u);
    auto* restoredTarget = restoredExclusion->waters.front().Resolve(*restored);
    ASSERT_NE(restoredTarget, nullptr);
    EXPECT_EQ(restoredTarget->name, "Target Water");
    const math::Vector3 expectedSize{3.0f, 4.0f, 5.0f};
    EXPECT_VEC3_NEAR(restoredExclusion->size, expectedSize, 1.0e-5f);
}

TEST_F(WaterExclusionTest, InteractionSettingsPersistWhileSaveAndPassiveCopyDropRuntimeState)
{
    scene::Scene source;
    auto& waterObject = source.CreateGameObject("Interaction Water");
    auto& water = waterObject.AddComponent<scene::WaterComponent>();
    water.interactionSettings.enabled = false;
    water.interactionSettings.motionInterval = 0.75f;
    water.interactionSettings.sedimentScattering = {0.2f, 0.12f, 0.06f};
    water.interactionRuntime.sceneGeneration = 44;
    water.interactionRuntime.owner = waterObject.GetID();
    water.interactionRuntime.ownerLifetime = source.GetComponentLifetime<scene::WaterComponent>(waterObject.GetID());
    water.interactionRuntime.revision = 9;
    water.interactionRuntime.pending.emplace_back();
    water.interactionRuntime.active.emplace_back();
    water.interactionRuntime.emitters[0] = {waterObject.GetID(), 13};
    water.interactionRuntime.snapshot = std::make_shared<scene::WaterInteractionFrame>();

    const std::string serialized = scene::SceneSerializer::SaveToText(source);
    ASSERT_FALSE(serialized.empty());
    auto restored = scene::SceneSerializer::LoadDataFromText(serialized, "");
    ASSERT_NE(restored, nullptr);
    auto* restoredObject = restored->Find("Interaction Water");
    ASSERT_NE(restoredObject, nullptr);
    auto* restoredWater = restoredObject->GetComponent<scene::WaterComponent>();
    ASSERT_NE(restoredWater, nullptr);
    EXPECT_FALSE(restoredWater->interactionSettings.enabled);
    EXPECT_FLOAT_EQ(restoredWater->interactionSettings.motionInterval, 0.75f);
    EXPECT_FLOAT_EQ(restoredWater->interactionSettings.sedimentScattering.x, 0.2f);
    EXPECT_TRUE(restoredWater->interactionRuntime.pending.empty());
    EXPECT_TRUE(restoredWater->interactionRuntime.active.empty());
    EXPECT_EQ(restoredWater->interactionRuntime.snapshot, nullptr);
    EXPECT_EQ(restoredWater->interactionRuntime.revision, 0u);

    std::string playDiagnostic;
    const std::string playBaseline = scene::SceneSerializer::CapturePassiveClone(source, playDiagnostic);
    ASSERT_FALSE(playBaseline.empty()) << playDiagnostic;
    auto playClone = scene::SceneSerializer::LoadFromText(playBaseline, nullptr, "");
    ASSERT_NE(playClone, nullptr);
    auto* playWaterObject = playClone->Find("Interaction Water");
    ASSERT_NE(playWaterObject, nullptr);
    const auto* playWater = playWaterObject->GetComponent<scene::WaterComponent>();
    ASSERT_NE(playWater, nullptr);
    EXPECT_FALSE(playWater->interactionSettings.enabled);
    EXPECT_FLOAT_EQ(playWater->interactionSettings.motionInterval, 0.75f);
    EXPECT_TRUE(playWater->interactionRuntime.pending.empty());
    EXPECT_TRUE(playWater->interactionRuntime.active.empty());
    EXPECT_EQ(playWater->interactionRuntime.snapshot, nullptr);
    EXPECT_EQ(playWater->interactionRuntime.sceneGeneration, 0u);

    scene::Scene destination;
    auto& copyObject = destination.CreateGameObject("Prefab Water Copy");
    destination.CopyComponentsFromPassive(source, waterObject.GetID(), copyObject.GetID());
    auto* copiedWater = copyObject.GetComponent<scene::WaterComponent>();
    ASSERT_NE(copiedWater, nullptr);
    EXPECT_FALSE(copiedWater->interactionSettings.enabled);
    EXPECT_FLOAT_EQ(copiedWater->interactionSettings.motionInterval, 0.75f);
    EXPECT_TRUE(copiedWater->interactionRuntime.pending.empty());
    EXPECT_TRUE(copiedWater->interactionRuntime.active.empty());
    EXPECT_EQ(copiedWater->interactionRuntime.snapshot, nullptr);
    EXPECT_EQ(copiedWater->interactionRuntime.sceneGeneration, 0u);
    EXPECT_EQ(copiedWater->interactionRuntime.ownerLifetime, 0u);
    EXPECT_EQ(copiedWater->interactionRuntime.revision, 0u);
    EXPECT_FALSE(copiedWater->interactionRuntime.owner.IsValid());
    for (const auto& emitter : copiedWater->interactionRuntime.emitters) {
        EXPECT_FALSE(emitter.id.IsValid());
        EXPECT_EQ(emitter.lifetime, 0u);
    }
}

} /// @note namespace fbzz::tests
