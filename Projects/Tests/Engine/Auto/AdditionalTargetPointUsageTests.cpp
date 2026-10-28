/// @file    AdditionalTargetPointUsageTests.cpp
/// @brief   Camera・Constraint・VFX・LODの配置点追従とGUID寿命を実System境界で検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#include <TestKit/TestKit.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/CameraRigComponents.hpp>
#include <Engine/Scene/Components/ConstraintComponents.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/LODGroupComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/ProceduralMeshComponent.hpp>
#include <Engine/Scene/Components/TargetPointComponent.hpp>
#include <Engine/Scene/Components/TrailComponent.hpp>
#include <Engine/Scene/Components/VFXBeamComponent.hpp>
#include <Engine/Scene/Components/VFXLineComponent.hpp>
#include <Engine/Scene/Fields/FlowField.hpp>
#include <Engine/Scene/Fields/FlowFieldEval.hpp>
#include <Engine/Scene/PrefabInstantiate.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/Systems/GameplayComponentSystems.hpp>
#include <Engine/Scene/Systems/LODSystem.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/Systems/VFXBeamSystem.hpp>
#include <Engine/Scene/Systems/VFXLineSystem.hpp>
#include <Math/MathUtils.hpp>
#include <Physics/World.hpp>
#include <array>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::tests {

class AdditionalTargetPointUsageTest : public testkit::Fixture {
protected:
    scene::Scene m_scene;
    physics::World m_world;
    template<typename System>
    void Tick()
    {
        SystemContext context{m_scene, m_world, nullptr, nullptr, 0.25f, 0.02f, true, true};
        System{}.Update(context);
    }
    scene::GameObject& Point(const char* name = "Target")
    {
        auto& point = m_scene.CreateGameObject(name); point.AddComponent<scene::TargetPointComponent>(); return point;
    }
};

TEST_F(AdditionalTargetPointUsageTest, CameraFollowsMarkerWithRotationOffsetAndIgnoresItsScale)
{
    auto& parent = m_scene.CreateGameObject("Parent"); parent.transform.position = {10.0f, 0.0f, 0.0f}; parent.transform.scale = {-2.0f, 3.0f, 0.5f};
    auto& camera = m_scene.CreateGameObject("Camera"); camera.SetParent(parent);
    auto& follow = camera.AddComponent<scene::CameraFollowComponent>();
    auto& point = Point(); point.transform.position = {3.0f, 2.0f, 4.0f}; point.transform.scale = {-4.0f, 0.0f, 7.0f};
    point.transform.rotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(90.0f));
    follow.target.guid = point.instanceId; follow.offset = {1.0f, 0.0f, 0.0f};
    follow.useTargetRotation = true; follow.lookAtTarget = false; follow.positionDamping = 0.0f;
    Tick<scene::CameraRigSystem>();
    EXPECT_VEC3_NEAR(camera.transform.worldPosition, (math::Vector3{3.0f, 2.0f, 3.0f}), 1.0e-4f);
    point.transform.position = {6.0f, 3.0f, 8.0f};
    Tick<scene::CameraRigSystem>();
    EXPECT_VEC3_NEAR(camera.transform.worldPosition, (math::Vector3{6.0f, 3.0f, 7.0f}), 1.0e-4f);
    EXPECT_VEC3_NEAR(follow.offset, (math::Vector3{1.0f, 0.0f, 0.0f}), testkit::kTolerance);
}

TEST_F(AdditionalTargetPointUsageTest, CameraRetainsMissingGuidAndDoesNotFollowSameNamedReplacement)
{
    auto& camera = m_scene.CreateGameObject("Camera"); auto& follow = camera.AddComponent<scene::CameraFollowComponent>();
    auto& point = Point(); point.transform.position = {5.0f, 1.0f, 2.0f};
    const auto guid = point.instanceId; const auto id = point.GetID();
    follow.target.guid = guid; follow.offset = math::Vector3::ZERO; follow.positionDamping = 0.0f; follow.lookAtTarget = false;
    Tick<scene::CameraRigSystem>(); const auto captured = camera.transform.worldPosition;
    ASSERT_TRUE(m_scene.DestroyGameObject(id)); Point().transform.position = {100.0f, 0.0f, 0.0f};
    Tick<scene::CameraRigSystem>();
    EXPECT_VEC3_NEAR(camera.transform.worldPosition, captured, testkit::kTolerance);
    EXPECT_EQ(follow.target.guid, guid); EXPECT_FALSE(follow.target.id.IsValid()); EXPECT_TRUE(follow.target.IsAssigned());
    const auto ownerGuid = camera.instanceId;
    auto loaded = scene::SceneSerializer::LoadDataFromText(scene::SceneSerializer::SaveToText(m_scene), "");
    ASSERT_NE(loaded, nullptr); auto* restored = loaded->FindByGuid(ownerGuid); ASSERT_NE(restored, nullptr);
    auto* saved = restored->GetComponent<scene::CameraFollowComponent>(); ASSERT_NE(saved, nullptr);
    EXPECT_EQ(saved->target.guid, guid); EXPECT_EQ(saved->target.Resolve(*loaded), nullptr);
}

TEST_F(AdditionalTargetPointUsageTest, CameraAndConstraintRejectInactiveSelfAndDescendantTargetsWithoutMoving)
{
    auto& owner = m_scene.CreateGameObject("Owner"); owner.transform.position = {1.0f, 2.0f, 3.0f};
    auto& camera = owner.AddComponent<scene::CameraFollowComponent>(); camera.offset = {2.0f, 0.0f, 0.0f}; camera.positionDamping = 0.0f;
    auto& constraint = owner.AddComponent<scene::TransformConstraintComponent>(); constraint.mode = scene::TransformConstraintMode::Position;
    auto& target = Point(); target.transform.position = {20.0f, 0.0f, 0.0f};
    camera.target.guid = target.instanceId; constraint.target.guid = target.instanceId; target.SetActive(false);
    Tick<scene::CameraRigSystem>(); Tick<scene::ConstraintSystem>();
    EXPECT_VEC3_NEAR(owner.transform.position, (math::Vector3{1.0f, 2.0f, 3.0f}), testkit::kTolerance);
    target.SetActive(true); target.SetParent(owner);
    Tick<scene::CameraRigSystem>(); Tick<scene::ConstraintSystem>();
    EXPECT_VEC3_NEAR(owner.transform.position, (math::Vector3{1.0f, 2.0f, 3.0f}), testkit::kTolerance);
    camera.target.guid = owner.instanceId; constraint.target.guid = owner.instanceId;
    Tick<scene::CameraRigSystem>(); Tick<scene::ConstraintSystem>();
    EXPECT_VEC3_NEAR(owner.transform.position, (math::Vector3{1.0f, 2.0f, 3.0f}), testkit::kTolerance);
}

TEST_F(AdditionalTargetPointUsageTest, InvalidPoseAndNoninvertibleParentLeaveFollowTransformsUnchanged)
{
    auto& parent = m_scene.CreateGameObject("Parent"); parent.transform.scale = {0.0f, 1.0f, 1.0f};
    auto& owner = m_scene.CreateGameObject("Owner"); owner.SetParent(parent); owner.transform.position = {1.0f, 2.0f, 3.0f};
    auto& camera = owner.AddComponent<scene::CameraFollowComponent>(); camera.positionDamping = 0.0f;
    auto& constraint = owner.AddComponent<scene::TransformConstraintComponent>(); constraint.mode = scene::TransformConstraintMode::Position;
    auto& point = Point(); point.transform.position = {10.0f, 0.0f, 0.0f}; camera.target.guid = point.instanceId; constraint.target.guid = point.instanceId;
    Tick<scene::CameraRigSystem>(); Tick<scene::ConstraintSystem>();
    EXPECT_VEC3_NEAR(owner.transform.position, (math::Vector3{1.0f, 2.0f, 3.0f}), testkit::kTolerance);
    parent.transform.scale = math::Vector3::ONE; point.transform.position.x = std::numeric_limits<float>::quiet_NaN();
    Tick<scene::CameraRigSystem>(); Tick<scene::ConstraintSystem>();
    EXPECT_VEC3_NEAR(owner.transform.position, (math::Vector3{1.0f, 2.0f, 3.0f}), testkit::kTolerance);
}

TEST_F(AdditionalTargetPointUsageTest, CloneForestCameraAndConstraintBindToClonedMarker)
{
    auto& owner = m_scene.CreateGameObject("Owner"); auto& point = Point();
    auto& camera = owner.AddComponent<scene::CameraFollowComponent>(); camera.target.guid = point.instanceId;
    camera.positionDamping = 0.0f; camera.lookAtTarget = false; camera.offset = math::Vector3::ZERO;
    owner.AddComponent<scene::TransformConstraintComponent>().target.guid = point.instanceId;
    scene::Scene destination; auto& copy = destination.CreateGameObject("Copy"); auto& pointCopy = destination.CreateGameObject("PointCopy");
    pointCopy.AddComponent<scene::TargetPointComponent>(); pointCopy.transform.position = {7.0f, 8.0f, 9.0f};
    destination.CopyComponentsFromPassive(m_scene, owner.GetID(), copy.GetID());
    const std::vector<std::pair<scene::EntityID, scene::EntityID>> replacements{{owner.GetID(), copy.GetID()}, {point.GetID(), pointCopy.GetID()}};
    const std::array<scene::EntityID, 1> copiedOwners{copy.GetID()};
    scene::RemapCopiedObjectReferences(m_scene, destination, replacements, copiedOwners);
    EXPECT_EQ(copy.GetComponent<scene::CameraFollowComponent>()->target.guid, pointCopy.instanceId);
    EXPECT_EQ(copy.GetComponent<scene::TransformConstraintComponent>()->target.guid, pointCopy.instanceId);
    SystemContext context{destination, m_world, nullptr, nullptr, 0.25f, 0.02f, true, true};
    scene::CameraRigSystem{}.Update(context);
    EXPECT_VEC3_NEAR(copy.transform.worldPosition, pointCopy.transform.worldPosition, testkit::kTolerance);
}

TEST_F(AdditionalTargetPointUsageTest, MissingExplicitSocketTargetDoesNotUseMatchingAncestorSocket)
{
    auto& parent = m_scene.CreateGameObject("Rig"); auto& socket = m_scene.CreateGameObject("Grip"); socket.SetParent(parent);
    socket.transform.position = {30.0f, 0.0f, 0.0f};
    auto& owner = m_scene.CreateGameObject("Item"); owner.SetParent(parent); owner.transform.position = {1.0f, 2.0f, 3.0f};
    auto& attachment = owner.AddComponent<scene::SocketAttachmentComponent>(); attachment.target.guid = "missing-rig-guid"; attachment.socketName = "Grip";
    Tick<scene::ConstraintSystem>();
    EXPECT_VEC3_NEAR(owner.transform.position, (math::Vector3{1.0f, 2.0f, 3.0f}), testkit::kTolerance);
    EXPECT_EQ(attachment.target.guid, "missing-rig-guid");
}

TEST_F(AdditionalTargetPointUsageTest, VfxBeamAndLineRetainMissingEndpointGuidAndHideOldGeometry)
{
    auto& start = Point("Start"); auto& end = Point("End"); end.transform.position = {0.0f, 0.0f, 10.0f};
    auto& owner = m_scene.CreateGameObject("Effects");
    auto& beam = owner.AddComponent<scene::VFXBeamComponent>(); beam.fromEntity.guid = start.instanceId; beam.toEntity.guid = end.instanceId;
    auto& trail = owner.AddComponent<scene::TrailComponent>();
    auto& line = owner.AddComponent<scene::VFXLineComponent>(); line.mode = scene::VFXLineMode::Beam; line.wobble = 0.0f;
    line.fromEntity.guid = start.instanceId; line.toEntity.guid = end.instanceId;
    Tick<scene::VFXBeamSystem>(); Tick<scene::VFXLineSystem>();
    ASSERT_FALSE(trail.beamPoints.empty()); EXPECT_VEC3_NEAR(trail.beamPoints.back(), end.transform.worldPosition, testkit::kTolerance);
    auto* geometry = owner.GetComponent<scene::ProceduralMeshComponent>(); ASSERT_NE(geometry, nullptr);
    ASSERT_TRUE(geometry->enabled); EXPECT_GT(geometry->builder.VertexCount(), 0u);
    const auto endGuid = end.instanceId; const auto endId = end.GetID(); const auto ownerGuid = owner.instanceId;
    ASSERT_TRUE(m_scene.DestroyGameObject(endId)); Point("End").transform.position = {50.0f, 0.0f, 0.0f};
    Tick<scene::VFXBeamSystem>(); Tick<scene::VFXLineSystem>();
    EXPECT_TRUE(trail.beamPoints.empty()); EXPECT_FALSE(trail.enabled); EXPECT_FALSE(geometry->enabled);
    EXPECT_EQ(beam.toEntity.guid, endGuid); EXPECT_EQ(line.toEntity.guid, endGuid);
    auto loaded = scene::SceneSerializer::LoadDataFromText(scene::SceneSerializer::SaveToText(m_scene), "");
    ASSERT_NE(loaded, nullptr); auto* restored = loaded->FindByGuid(ownerGuid); ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->GetComponent<scene::VFXBeamComponent>()->toEntity.guid, endGuid);
    EXPECT_EQ(restored->GetComponent<scene::VFXLineComponent>()->toEntity.guid, endGuid);
}

TEST_F(AdditionalTargetPointUsageTest, LodCenterOnlyChangesDistanceAndMissingCenterRestoresEveryRenderer)
{
    auto& camera = m_scene.CreateGameObject("Camera"); camera.AddComponent<scene::CameraComponent>();
    auto& owner = m_scene.CreateGameObject("LOD"); owner.transform.position = {0.0f, 0.0f, 1000.0f}; owner.transform.scale = {-2.0f, 1.0f, 1.0f};
    auto& high = m_scene.CreateGameObject("High"); auto& highRenderer = high.AddComponent<scene::MeshRenderer>();
    auto& low = m_scene.CreateGameObject("Low"); auto& lowRenderer = low.AddComponent<scene::MeshRenderer>();
    auto& group = owner.AddComponent<scene::LODGroupComponent>(); group.fadeDuration = 0.0f;
    group.levels = {{0.5f, {{high.instanceId, high.GetID()}}}, {0.05f, {{low.instanceId, low.GetID()}}}};
    Tick<scene::LODSystem>(); EXPECT_EQ(group.activeLevel, 1); EXPECT_FALSE(highRenderer.lodVisible); EXPECT_TRUE(lowRenderer.lodVisible);
    auto& center = Point("Center"); center.transform.position = {0.0f, 0.0f, 1.0f}; center.transform.scale = {100.0f, 100.0f, 100.0f};
    group.centerSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS; group.centerTarget.guid = center.instanceId;
    Tick<scene::LODSystem>(); EXPECT_EQ(group.activeLevel, 0); EXPECT_TRUE(highRenderer.lodVisible); EXPECT_FALSE(lowRenderer.lodVisible);
    center.transform.scale = {0.01f, 0.01f, 0.01f}; Tick<scene::LODSystem>(); EXPECT_EQ(group.activeLevel, 0);
    center.transform.position.z = 100.0f; Tick<scene::LODSystem>(); EXPECT_EQ(group.activeLevel, 1);
    const auto guid = center.instanceId; const auto id = center.GetID(); ASSERT_TRUE(m_scene.DestroyGameObject(id));
    Tick<scene::LODSystem>();
    EXPECT_EQ(group.centerTarget.guid, guid); EXPECT_EQ(group.activeLevel, -1); EXPECT_EQ(group.fadingLevel, -1);
    EXPECT_TRUE(highRenderer.lodVisible); EXPECT_TRUE(lowRenderer.lodVisible); EXPECT_FLOAT_EQ(group.size, 1.0f);
}

TEST_F(AdditionalTargetPointUsageTest, LightAndFlowAnchorUseExistingConstraintPoseWithoutChangingNumericSettings)
{
    auto& point = Point(); point.transform.position = {10.0f, 0.0f, 0.0f};
    point.transform.rotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(90.0f));
    auto& light = m_scene.CreateGameObject("Light"); auto& emission = light.AddComponent<scene::LightComponent>(); emission.intensity = 12.0f;
    auto& aim = light.AddComponent<scene::TransformConstraintComponent>(); aim.target.guid = point.instanceId;
    aim.mode = scene::TransformConstraintMode::LookAt; aim.maintainOffset = false;
    auto& flowOwner = m_scene.CreateGameObject("Flow"); auto& flow = flowOwner.AddComponent<scene::FlowField>(); flow.forces[0].strength = 7.0f;
    auto& pose = flowOwner.AddComponent<scene::TransformConstraintComponent>(); pose.target.guid = point.instanceId;
    pose.mode = scene::TransformConstraintMode::Parent; pose.maintainOffset = false;
    Tick<scene::ConstraintSystem>();
    EXPECT_VEC3_NEAR(light.transform.worldRotation * math::Vector3::FORWARD, math::Vector3::RIGHT, 1.0e-4f);
    EXPECT_FLOAT_EQ(emission.intensity, 12.0f); EXPECT_FLOAT_EQ(flow.forces[0].strength, 7.0f);
    std::vector<scene::ActiveFlowField> fields; scene::GatherFlowFields(m_scene, fields); ASSERT_FALSE(fields.empty());
    EXPECT_VEC3_NEAR(fields[0].position, point.transform.worldPosition, testkit::kTolerance);
    EXPECT_FLOAT_EQ(fields[0].strength, 7.0f);
    EXPECT_VEC3_NEAR(flowOwner.transform.worldPosition, point.transform.worldPosition, testkit::kTolerance);
}

} /// @note namespace fbzz::tests
