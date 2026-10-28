/// @file    ControlPointResolverTests.cpp
/// @brief   共通 ControlPoint 参照・owner-relative 数値解決の契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#include <TestKit/TestKit.hpp>

#include <Engine/Scene/Components/TargetPointComponent.hpp>
#include <Engine/Scene/Components/SplineComponents.hpp>
#include <Engine/Scene/ControlPointResolver.hpp>
#include <Engine/Scene/PrefabInstantiate.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Math/MathUtils.hpp>
#include <array>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::tests {
namespace {

class ControlPointResolverTest : public testkit::Fixture {
protected:
    scene::Scene m_scene;
};

TEST_F(ControlPointResolverTest, DescendantPositionMatchesEngineNestedSignedScaleRotationOrder)
{
    auto& owner = m_scene.CreateGameObject("Owner");
    auto& scaledParent = m_scene.CreateGameObject("ScaledParent");
    auto& rotatedParent = m_scene.CreateGameObject("RotatedParent");
    auto& target = m_scene.CreateGameObject("Target");
    scaledParent.SetParent(owner);
    rotatedParent.SetParent(scaledParent);
    target.SetParent(rotatedParent);
    ASSERT_EQ(scaledParent.GetParent(), &owner);
    ASSERT_EQ(rotatedParent.GetParent(), &scaledParent);
    ASSERT_EQ(target.GetParent(), &rotatedParent);

    owner.transform.position = {10000000.0f, -4000000.0f, 2000000.0f};
    owner.transform.scale = {2.0f, 1.0f, 1.0f};
    scaledParent.transform.scale = {-2.0f, 1.0f, 1.0f};
    rotatedParent.transform.rotation = math::Quaternion::FromAxisAngle(
        math::Vector3::UP, math::ToRad(90.0f));
    target.transform.position = {1.0f, 0.0f, 0.0f};
    target.AddComponent<scene::TargetPointComponent>();
    scene::FlushWorldTransforms(m_scene);

    scene::ObjectReference reference{target.instanceId, target.GetID(),
                                     m_scene.GetRenderSceneGeneration()};
    const std::array<scene::ControlPointBinding, 1> bindings{{{41, &reference}}};
    std::vector<scene::ResolvedControlPoint> resolved;
    scene::ControlPointResolveStatus status{};
    uint64_t signature = 0;

    ASSERT_TRUE(scene::ResolveControlPointBindings(m_scene, owner.GetID(), bindings,
        true, resolved, status, signature));
    ASSERT_EQ(status, scene::ControlPointResolveStatus::SUCCESS);
    ASSERT_EQ(resolved.size(), 1u);
    EXPECT_EQ(resolved[0].stableId, 41u);
    EXPECT_EQ(resolved[0].targetId, target.GetID());
    EXPECT_VEC3_NEAR(resolved[0].ownerLocalPosition,
                     (math::Vector3{0.0f, 0.0f, 4.0f}), testkit::kTolerance);
    EXPECT_NE(signature, 0u);
}

TEST_F(ControlPointResolverTest, MissingGuidIsRetainedAndFailureLeavesNumericOutputUnchanged)
{
    auto& owner = m_scene.CreateGameObject("Owner");
    auto& unrelated = m_scene.CreateGameObject("Unrelated");
    scene::ObjectReference reference{"missing-point-guid", unrelated.GetID(),
                                     m_scene.GetRenderSceneGeneration()};
    const std::array<scene::ControlPointBinding, 1> bindings{{{7, &reference}}};
    std::vector<scene::ResolvedControlPoint> resolved{{
        99, unrelated.GetID(), 13, {1.0f, 2.0f, 3.0f}, {4.0f, 5.0f, 6.0f},
        math::Quaternion::Identity(), math::Vector3::ONE}};
    const auto before = resolved;
    scene::ControlPointResolveStatus status{};
    uint64_t signature = 5;

    EXPECT_FALSE(scene::ResolveControlPointBindings(m_scene, owner.GetID(), bindings,
        false, resolved, status, signature));
    EXPECT_EQ(status, scene::ControlPointResolveStatus::UNRESOLVED_REFERENCE);
    EXPECT_EQ(reference.guid, "missing-point-guid");
    EXPECT_FALSE(reference.id.IsValid());
    EXPECT_EQ(reference.sceneGeneration, m_scene.GetRenderSceneGeneration());
    EXPECT_EQ(signature, 0u);
    ASSERT_EQ(resolved.size(), before.size());
    EXPECT_EQ(resolved[0].stableId, before[0].stableId);
    EXPECT_VEC3_NEAR(resolved[0].ownerLocalPosition,
                     before[0].ownerLocalPosition, testkit::kTolerance);
}

TEST_F(ControlPointResolverTest, EmptyGuidRemainsNullDespiteValidCachedEntity)
{
    auto& owner = m_scene.CreateGameObject("Owner");
    auto& target = m_scene.CreateGameObject("Target");
    target.AddComponent<scene::TargetPointComponent>();
    scene::ObjectReference reference{"", target.GetID(), m_scene.GetRenderSceneGeneration()};
    const std::array<scene::ControlPointBinding, 1> bindings{{{19, &reference}}};
    std::vector<scene::ResolvedControlPoint> resolved{{
        99, target.GetID(), 13, {1.0f, 2.0f, 3.0f}, {4.0f, 5.0f, 6.0f},
        math::Quaternion::Identity(), math::Vector3::ONE}};
    const auto before = resolved;
    scene::ControlPointResolveStatus status{};
    uint64_t signature = 7;

    EXPECT_FALSE(scene::ResolveControlPointBindings(m_scene, owner.GetID(), bindings,
        true, resolved, status, signature));
    EXPECT_EQ(status, scene::ControlPointResolveStatus::NULL_REFERENCE);
    EXPECT_TRUE(reference.guid.empty());
    EXPECT_FALSE(reference.id.IsValid());
    EXPECT_EQ(reference.sceneGeneration, m_scene.GetRenderSceneGeneration());
    EXPECT_EQ(signature, 0u);
    ASSERT_EQ(resolved.size(), before.size());
    EXPECT_EQ(resolved[0].stableId, before[0].stableId);
    EXPECT_VEC3_NEAR(resolved[0].ownerLocalPosition,
                     before[0].ownerLocalPosition, testkit::kTolerance);
}

TEST_F(ControlPointResolverTest, StaleSceneCacheResolvesOnlyTheCurrentScenesMatchingGuid)
{
    scene::Scene oldScene;
    auto& oldTarget = oldScene.CreateGameObject("OldTarget");
    oldTarget.instanceId = "shared-target-guid";
    oldTarget.AddComponent<scene::TargetPointComponent>();

    auto& decoy = m_scene.CreateGameObject("Decoy");
    auto& target = m_scene.CreateGameObject("CurrentTarget");
    target.instanceId = oldTarget.instanceId;
    target.AddComponent<scene::TargetPointComponent>();
    auto& owner = m_scene.CreateGameObject("Owner");
    ASSERT_EQ(oldTarget.GetID().index, decoy.GetID().index);
    scene::ObjectReference reference{oldTarget.instanceId, oldTarget.GetID(),
                                     oldScene.GetRenderSceneGeneration()};
    const std::array<scene::ControlPointBinding, 1> bindings{{{23, &reference}}};
    std::vector<scene::ResolvedControlPoint> resolved;
    scene::ControlPointResolveStatus status{};
    uint64_t signature = 0;

    ASSERT_TRUE(scene::ResolveControlPointBindings(m_scene, owner.GetID(), bindings,
        true, resolved, status, signature));
    ASSERT_EQ(resolved.size(), 1u);
    EXPECT_EQ(resolved[0].targetId, target.GetID());
    EXPECT_NE(resolved[0].targetId, decoy.GetID());
    EXPECT_EQ(reference.id, target.GetID());
    EXPECT_EQ(reference.sceneGeneration, m_scene.GetRenderSceneGeneration());
}

TEST_F(ControlPointResolverTest, EmptyGuidSurvivesSceneSaveLoadAndTypedCloneRemap)
{
    auto& target = m_scene.CreateGameObject("Target");
    target.AddComponent<scene::TargetPointComponent>();
    auto& owner = m_scene.CreateGameObject("Owner");
    scene::SplineComponent spline;
    spline.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    scene::SplineControlPointBinding binding;
    binding.id = 31;
    binding.target.id = target.GetID();
    binding.target.sceneGeneration = m_scene.GetRenderSceneGeneration();
    spline.pointBindings.push_back(binding);
    owner.AddComponent<scene::SplineComponent>(std::move(spline));

    const std::string serialized = scene::SceneSerializer::SaveToText(m_scene);
    auto restored = scene::SceneSerializer::LoadDataFromText(serialized, "");
    ASSERT_NE(restored, nullptr);
    auto* restoredOwner = restored->FindByGuid(owner.instanceId);
    ASSERT_NE(restoredOwner, nullptr);
    const auto* restoredSpline = restoredOwner->GetComponent<scene::SplineComponent>();
    ASSERT_NE(restoredSpline, nullptr);
    ASSERT_EQ(restoredSpline->pointBindings.size(), 1u);
    EXPECT_TRUE(restoredSpline->pointBindings[0].target.guid.empty());
    EXPECT_FALSE(restoredSpline->pointBindings[0].target.id.IsValid());

    scene::Scene destination;
    auto& destinationTarget = destination.CreateGameObject("TargetClone");
    destinationTarget.AddComponent<scene::TargetPointComponent>();
    auto& destinationOwner = destination.CreateGameObject("OwnerClone");
    destination.CopyComponentsFromPassive(m_scene, owner.GetID(), destinationOwner.GetID());
    const std::vector<std::pair<scene::EntityID, scene::EntityID>> replacements{
        {owner.GetID(), destinationOwner.GetID()},
        {target.GetID(), destinationTarget.GetID()}};
    const std::array<scene::EntityID, 1> copiedOwners{{destinationOwner.GetID()}};
    scene::RemapCopiedObjectReferences(m_scene, destination, replacements, copiedOwners);

    const auto* clonedSpline = destinationOwner.GetComponent<scene::SplineComponent>();
    ASSERT_NE(clonedSpline, nullptr);
    ASSERT_EQ(clonedSpline->pointBindings.size(), 1u);
    EXPECT_TRUE(clonedSpline->pointBindings[0].target.guid.empty());
    EXPECT_FALSE(clonedSpline->pointBindings[0].target.id.IsValid());
    EXPECT_EQ(clonedSpline->pointBindings[0].target.sceneGeneration,
              destination.GetRenderSceneGeneration());
}

TEST_F(ControlPointResolverTest, WaterAndSplineBindingsRejectDuplicateTargets)
{
    auto& owner = m_scene.CreateGameObject("Owner");
    auto& target = m_scene.CreateGameObject("Target");
    target.AddComponent<scene::TargetPointComponent>();
    scene::ObjectReference reference{target.instanceId, target.GetID(),
                                     m_scene.GetRenderSceneGeneration()};
    const std::array<scene::ControlPointBinding, 2> bindings{{
        {11, &reference}, {12, &reference}}};
    std::vector<scene::ResolvedControlPoint> resolved{{
        99, owner.GetID(), 0, {1.0f, 2.0f, 3.0f}, {4.0f, 5.0f, 6.0f},
        math::Quaternion::Identity(), math::Vector3::ONE}};
    scene::ControlPointResolveStatus status{};
    uint64_t signature = 0;

    EXPECT_FALSE(scene::ResolveControlPointBindings(m_scene, owner.GetID(), bindings,
        true, resolved, status, signature));
    EXPECT_EQ(status, scene::ControlPointResolveStatus::DUPLICATE_TARGET);
    ASSERT_EQ(resolved.size(), 1u);
    EXPECT_EQ(resolved[0].stableId, 99u);
    EXPECT_EQ(signature, 0u);
}

TEST_F(ControlPointResolverTest, TargetPointRegistrationAndDisplaySettingsRoundTrip)
{
    auto& target = m_scene.CreateGameObject("TargetPoint");
    scene::TargetPointComponent marker;
    marker.visible = false;
    marker.color = {0.1f, 0.2f, 0.7f, 0.8f};
    marker.screenSize = 17.0f;
    target.AddComponent<scene::TargetPointComponent>(marker);

    const std::string text = scene::SceneSerializer::SaveToText(m_scene);
    ASSERT_FALSE(text.empty());
    auto restored = scene::SceneSerializer::LoadDataFromText(text, "");
    ASSERT_NE(restored, nullptr);
    scene::GameObject* restoredTarget = restored->FindByGuid(target.instanceId);
    ASSERT_NE(restoredTarget, nullptr);
    const auto* restoredMarker = restoredTarget->GetComponent<scene::TargetPointComponent>();
    ASSERT_NE(restoredMarker, nullptr);
    EXPECT_FALSE(restoredMarker->visible);
    EXPECT_VEC4_NEAR(restoredMarker->color, (math::Vector4{0.1f, 0.2f, 0.7f, 0.8f}),
                     testkit::kTolerance);
    EXPECT_FLOAT_EQ(restoredMarker->screenSize, 17.0f);
}

} /// @note namespace
} /// @note namespace fbzz::tests
