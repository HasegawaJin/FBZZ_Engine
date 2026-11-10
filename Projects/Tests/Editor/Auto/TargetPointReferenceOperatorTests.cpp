/// @file    TargetPointReferenceOperatorTests.cpp
/// @brief   共通consumer接続とanchor作成を実OpとChecked Undo境界で検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Tools/TargetPointReferenceEditing.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Engine/Scene/Components/BoneComponent.hpp>
#include <Engine/Scene/Components/CameraRigComponents.hpp>
#include <Engine/Scene/Components/ConstraintComponents.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/LODGroupComponent.hpp>
#include <Engine/Scene/Components/MotionWarpComponent.hpp>
#include <Engine/Scene/Components/NavMeshAgentComponent.hpp>
#include <Engine/Scene/Components/NavMeshPatrolComponent.hpp>
#include <Engine/Scene/Components/TargetPointComponent.hpp>
#include <Engine/Scene/Components/VFXBeamComponent.hpp>
#include <Engine/Scene/Components/VFXLineComponent.hpp>
#include <Engine/Scene/Fields/FlowField.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Math/MathUtils.hpp>
#include <string>
#include <utility>

namespace fbzz::tests {

class TargetPointReferenceOperatorTest : public testkit::EditorFixture {
protected:
    editor::OperatorRegistry m_registry;
    editor::UndoStack m_undo;
    void SetUp() override
    {
        EditorFixture::SetUp(); AttachScene();
        Context().operators = &m_registry; Context().undoStack = &m_undo;
        editor::RegisterTargetPointReferenceOperators(m_registry);
    }
    void TearDown() override { m_undo.Clear(); EditorFixture::TearDown(); }
    scene::Scene& World() { return *AttachedScene(); }
    editor::OpArgs Args(scene::GameObject& owner, const char* kind, const char* role = "target")
    {
        editor::OpArgs args; args.Set("node", owner.instanceId); args.Set("kind", std::string(kind));
        args.Set("role", std::string(role)); return args;
    }
    editor::OpResult Invoke(const char* op, const editor::OpArgs& args)
    {
        return editor::InvokeOperator(Context(), op, args);
    }
};

TEST_F(TargetPointReferenceOperatorTest, ExistingConsumersAcceptNormalBoneAndUndoOnlyTheirReference)
{
    auto& owner = World().CreateGameObject("Consumers");
    auto& target = World().CreateGameObject("Bone"); target.AddComponent<scene::BoneComponent>();
    auto& camera = owner.AddComponent<scene::CameraFollowComponent>();
    owner.AddComponent<scene::TransformConstraintComponent>();
    owner.AddComponent<scene::VFXBeamComponent>(); owner.AddComponent<scene::VFXLineComponent>();
    owner.AddComponent<scene::NavMeshAgentComponent>();
    auto cameraArgs = Args(owner, "camera_follow"); cameraArgs.Set("target", target.instanceId);
    ASSERT_TRUE(Invoke("target_point.bind_reference", cameraArgs).ok);
    ASSERT_EQ(camera.target.guid, target.instanceId);
    camera.offset = {7.0f, 8.0f, 9.0f};
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_TRUE(camera.target.guid.empty());
    EXPECT_VEC3_NEAR(camera.offset, (math::Vector3{7.0f, 8.0f, 9.0f}), testkit::kTolerance);
    ASSERT_TRUE(m_undo.Redo());
    EXPECT_EQ(camera.target.Resolve(World()), &target);
    for (const auto* kind : {"constraint", "vfx_beam", "vfx_line", "nav_agent"}) {
        auto args = Args(owner, kind, std::string(kind).find("vfx_") == 0 ? "to" : "target");
        args.Set("target", target.instanceId);
        ASSERT_TRUE(Invoke("target_point.bind_reference", args).ok) << kind;
    }
    EXPECT_EQ(owner.GetComponent<scene::TransformConstraintComponent>()->target.guid, target.instanceId);
    EXPECT_EQ(owner.GetComponent<scene::VFXBeamComponent>()->toEntity.guid, target.instanceId);
    EXPECT_EQ(owner.GetComponent<scene::VFXLineComponent>()->toEntity.guid, target.instanceId);
    EXPECT_EQ(owner.GetComponent<scene::NavMeshAgentComponent>()->target.guid, target.instanceId);
}

TEST_F(TargetPointReferenceOperatorTest, CreateAndBindUsesOneUndoAndRetainsGuidAcrossRedo)
{
    auto& owner = World().CreateGameObject("Beam"); owner.AddComponent<scene::VFXBeamComponent>();
    owner.transform.position = {100000000.0f, -20000000.0f, 30000000.0f}; owner.transform.scale = {-2.0f, 3.0f, 0.5f};
    auto args = Args(owner, "vfx_beam", "from"); args.Set("localPosition", math::Vector3{0.125f, 1.0f, -2.0f});
    ASSERT_TRUE(Invoke("target_point.create_and_bind", args).ok);
    const auto guid = owner.GetComponent<scene::VFXBeamComponent>()->fromEntity.guid;
    auto* point = World().FindByGuid(guid); ASSERT_NE(point, nullptr);
    EXPECT_EQ(point->GetParent(), &owner);
    EXPECT_VEC3_NEAR(point->transform.position, (math::Vector3{0.125f, 1.0f, -2.0f}), testkit::kTolerance);
    EXPECT_NE(point->GetComponent<scene::TargetPointComponent>(), nullptr);
    EXPECT_EQ(m_undo.GetHistorySize(), 1u);
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(World().FindByGuid(guid), nullptr);
    EXPECT_TRUE(owner.GetComponent<scene::VFXBeamComponent>()->fromEntity.guid.empty());
    ASSERT_TRUE(m_undo.Redo());
    EXPECT_NE(World().FindByGuid(guid), nullptr);
    EXPECT_EQ(owner.GetComponent<scene::VFXBeamComponent>()->fromEntity.guid, guid);
}

TEST_F(TargetPointReferenceOperatorTest, SameOwnerSecondEndpointProtectsCreatedSharedPointFromUndo)
{
    auto& owner = World().CreateGameObject("Beam"); auto& beam = owner.AddComponent<scene::VFXBeamComponent>();
    auto args = Args(owner, "vfx_beam", "from");
    ASSERT_TRUE(Invoke("target_point.create_and_bind", args).ok);
    const auto guid = beam.fromEntity.guid;
    beam.toEntity.guid = guid;
    const auto cursor = m_undo.GetCursor();
    EXPECT_FALSE(m_undo.Undo());
    EXPECT_EQ(m_undo.GetCursor(), cursor);
    EXPECT_NE(World().FindByGuid(guid), nullptr);
    EXPECT_EQ(beam.fromEntity.guid, guid);
}

TEST_F(TargetPointReferenceOperatorTest, NewPatrolReferenceProtectsCreatedPointFromUndo)
{
    auto& owner = World().CreateGameObject("Beam"); auto& beam = owner.AddComponent<scene::VFXBeamComponent>();
    ASSERT_TRUE(Invoke("target_point.create_and_bind", Args(owner, "vfx_beam", "from")).ok);
    const auto guid = beam.fromEntity.guid;
    auto& route = World().CreateGameObject("Route"); auto& patrol = route.AddComponent<scene::NavMeshPatrolComponent>();
    patrol.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    auto& binding = patrol.waypointBindings.emplace_back(); binding.id = 17; binding.target.guid = guid;
    EXPECT_FALSE(m_undo.Undo());
    EXPECT_NE(World().FindByGuid(guid), nullptr);
    EXPECT_EQ(beam.fromEntity.guid, guid);
}

TEST_F(TargetPointReferenceOperatorTest, EditingCreatedPointOrReplacingConsumerRejectsUndo)
{
    auto& owner = World().CreateGameObject("Beam"); owner.AddComponent<scene::VFXBeamComponent>();
    ASSERT_TRUE(Invoke("target_point.create_and_bind", Args(owner, "vfx_beam", "from")).ok);
    const auto guid = owner.GetComponent<scene::VFXBeamComponent>()->fromEntity.guid;
    auto* point = World().FindByGuid(guid); ASSERT_NE(point, nullptr);
    point->GetComponent<scene::TargetPointComponent>()->screenSize += 0.125f;
    EXPECT_FALSE(m_undo.Undo());
    point->GetComponent<scene::TargetPointComponent>()->screenSize = scene::TargetPointComponent{}.screenSize;
    owner.RemoveComponent<scene::VFXBeamComponent>();
    owner.AddComponent<scene::VFXBeamComponent>().fromEntity.guid = guid;
    EXPECT_FALSE(m_undo.Undo());
    EXPECT_NE(World().FindByGuid(guid), nullptr);
}

TEST_F(TargetPointReferenceOperatorTest, PrefabStructureAndInvalidPointFailWithoutCreatingHistory)
{
    auto& owner = World().CreateGameObject("Prefab Beam"); owner.AddComponent<scene::VFXBeamComponent>();
    owner.prefabAssetPath = "guid:prefab";
    const auto count = World().GameObjectCount();
    EXPECT_FALSE(Invoke("target_point.create_and_bind", Args(owner, "vfx_beam", "from")).ok);
    EXPECT_EQ(World().GameObjectCount(), count);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    auto args = Args(owner, "vfx_beam", "from"); args.Set("target", std::string("missing-target"));
    EXPECT_FALSE(Invoke("target_point.bind_reference", args).ok);
    EXPECT_TRUE(owner.GetComponent<scene::VFXBeamComponent>()->fromEntity.guid.empty());
}

TEST_F(TargetPointReferenceOperatorTest, MissingGuidClearAndUndoPreserveTheUnresolvedGuid)
{
    auto& owner = World().CreateGameObject("Camera"); auto& camera = owner.AddComponent<scene::CameraFollowComponent>();
    camera.target.guid = "missing-target-guid";
    auto args = Args(owner, "camera_follow"); args.Set("target", std::string{});
    ASSERT_TRUE(Invoke("target_point.bind_reference", args).ok);
    EXPECT_TRUE(camera.target.guid.empty());
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(camera.target.guid, "missing-target-guid");
    EXPECT_EQ(camera.target.Resolve(World()), nullptr);
    EXPECT_EQ(camera.target.guid, "missing-target-guid");
}

TEST_F(TargetPointReferenceOperatorTest, PersistentBindingRejectsRuntimeGeneratedTargetAndOwnerWithoutChanges)
{
    auto& owner = World().CreateGameObject("Beam"); auto& beam = owner.AddComponent<scene::VFXBeamComponent>();
    auto& runtimePoint = World().CreateGameObject("Runtime point"); runtimePoint.SetRuntimeGenerated(true);
    runtimePoint.AddComponent<scene::TargetPointComponent>();
    auto args = Args(owner, "vfx_beam", "from"); args.Set("target", runtimePoint.instanceId);
    EXPECT_FALSE(Invoke("target_point.bind_reference", args).ok);
    EXPECT_TRUE(beam.fromEntity.guid.empty()); EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    owner.SetRuntimeGenerated(true);
    const auto count = World().GameObjectCount();
    EXPECT_FALSE(Invoke("target_point.create_and_bind", Args(owner, "vfx_beam", "from")).ok);
    EXPECT_EQ(World().GameObjectCount(), count); EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    owner.SetRuntimeGenerated(false);
    auto& parent = World().CreateGameObject("Runtime hierarchy"); parent.SetRuntimeGenerated(true); owner.SetParent(parent);
    const auto parentedCount = World().GameObjectCount();
    EXPECT_FALSE(Invoke("target_point.create_and_bind", Args(owner, "vfx_beam", "from")).ok);
    EXPECT_EQ(World().GameObjectCount(), parentedCount); EXPECT_EQ(m_undo.GetHistorySize(), 0u);
}

TEST_F(TargetPointReferenceOperatorTest, IkConnectionKeepsChainLabelOffsetAndRejectsChangedChainIdentity)
{
    auto& owner = World().CreateGameObject("Rig"); auto& solver = owner.AddComponent<scene::IKSolverComponent>();
    auto& chain = solver.chains.emplace_back(); chain.targetName = "Hand"; chain.boneNames = {"Arm", "Forearm", "Hand"};
    chain.targetOffset = {1.0f, 2.0f, 3.0f};
    auto& target = World().CreateGameObject("Grip"); target.AddComponent<scene::BoneComponent>();
    auto args = Args(owner, "ik"); args.Set("target", target.instanceId); args.Set("chainIndex", 0);
    ASSERT_TRUE(Invoke("target_point.bind_reference", args).ok);
    EXPECT_EQ(chain.targetName, "Hand"); EXPECT_EQ(chain.targetEntity, target.GetID());
    chain.weight = 0.4f;
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_FLOAT_EQ(chain.weight, 0.4f);
    EXPECT_VEC3_NEAR(chain.targetOffset, (math::Vector3{1.0f, 2.0f, 3.0f}), testkit::kTolerance);
    ASSERT_TRUE(m_undo.Redo());
    chain.boneNames[0] = "Other Arm";
    EXPECT_FALSE(m_undo.Undo());
    EXPECT_EQ(chain.targetReference.guid, target.instanceId);
}

TEST_F(TargetPointReferenceOperatorTest, LodAndMotionWarpRequireMarkersAndRestoreOnlyAuthoringSource)
{
    auto& owner = World().CreateGameObject("Owner"); auto& lod = owner.AddComponent<scene::LODGroupComponent>();
    auto& warp = owner.AddComponent<scene::MotionWarpComponent>();
    auto& target = World().CreateGameObject("Point");
    auto args = Args(owner, "lod_center"); args.Set("target", target.instanceId);
    EXPECT_FALSE(Invoke("target_point.bind_reference", args).ok);
    target.AddComponent<scene::TargetPointComponent>();
    ASSERT_TRUE(Invoke("target_point.bind_reference", args).ok);
    EXPECT_EQ(lod.centerSourceMode, scene::ControlPointSourceMode::POINT_OBJECTS);
    lod.size = 8.0f; lod.activeLevel = 3;
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(lod.centerSourceMode, scene::ControlPointSourceMode::INLINE);
    EXPECT_FLOAT_EQ(lod.size, 8.0f); EXPECT_EQ(lod.activeLevel, 3);
    args = Args(owner, "motion_warp"); args.Set("target", target.instanceId); args.Set("mode", std::string("LIVE"));
    ASSERT_TRUE(Invoke("target_point.bind_reference", args).ok);
    warp.target.remaining = 0.75f; warp.runtimeWarpCount = 17;
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_TRUE(warp.targetReference.guid.empty());
    EXPECT_EQ(warp.targetPointMode, scene::MotionWarpPointMode::CAPTURED);
    EXPECT_FLOAT_EQ(warp.target.remaining, 0.75f); EXPECT_EQ(warp.runtimeWarpCount, 17u);
}

TEST_F(TargetPointReferenceOperatorTest, PoseDriversRejectDescendantTargets)
{
    auto& owner = World().CreateGameObject("Owner"); owner.AddComponent<scene::CameraFollowComponent>();
    owner.AddComponent<scene::TransformConstraintComponent>(); owner.AddComponent<scene::NavMeshAgentComponent>();
    owner.AddComponent<scene::MotionWarpComponent>();
    auto& target = World().CreateGameObject("Child"); target.SetParent(owner); target.AddComponent<scene::TargetPointComponent>();
    for (const auto* kind : {"camera_follow", "constraint", "nav_agent", "motion_warp"}) {
        auto args = Args(owner, kind); args.Set("target", target.instanceId);
        if (std::string(kind) == "motion_warp") args.Set("mode", std::string("LIVE"));
        EXPECT_FALSE(Invoke("target_point.bind_reference", args).ok) << kind;
    }
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
}

TEST_F(TargetPointReferenceOperatorTest, PoseDriverCreatesIndependentSiblingWithSignedParentTransform)
{
    auto& parent = World().CreateGameObject("Parent");
    parent.transform.position = {100000000.0f, -20000000.0f, 30000000.0f}; parent.transform.scale = {-2.0f, 3.0f, 0.5f};
    auto& owner = World().CreateGameObject("Camera"); owner.SetParent(parent);
    owner.transform.position = {4.0f, 5.0f, 6.0f}; owner.transform.scale = {2.0f, 1.0f, -1.0f};
    owner.transform.rotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(90.0f));
    owner.AddComponent<scene::CameraFollowComponent>();
    auto args = Args(owner, "camera_follow"); args.Set("localPosition", math::Vector3{1.0f, 0.0f, 0.0f});
    ASSERT_TRUE(Invoke("target_point.create_and_bind", args).ok);
    auto* point = World().FindByGuid(owner.GetComponent<scene::CameraFollowComponent>()->target.guid);
    ASSERT_NE(point, nullptr); EXPECT_EQ(point->GetParent(), &parent);
    EXPECT_VEC3_NEAR(point->transform.position, (math::Vector3{4.0f, 5.0f, 14.0f}), 1.0e-4f);
    EXPECT_EQ(m_undo.GetHistorySize(), 1u);
    ASSERT_TRUE(m_undo.Undo()); ASSERT_TRUE(m_undo.Redo());
}

TEST_F(TargetPointReferenceOperatorTest, LightAnchorAddsConstraintAndIndependentPointInOneUndo)
{
    auto& owner = World().CreateGameObject("Light"); auto& light = owner.AddComponent<scene::LightComponent>();
    light.intensity = 12.0f;
    editor::OpArgs args; args.Set("node", owner.instanceId); args.Set("kind", std::string("light"));
    args.Set("target", std::string{}); args.Set("mode", std::string("Position"));
    ASSERT_TRUE(Invoke("target_point.configure_anchor", args).ok);
    auto* constraint = owner.GetComponent<scene::TransformConstraintComponent>(); ASSERT_NE(constraint, nullptr);
    const auto guid = constraint->target.guid;
    auto* point = World().FindByGuid(guid); ASSERT_NE(point, nullptr); EXPECT_EQ(point->GetParent(), nullptr);
    EXPECT_FLOAT_EQ(light.intensity, 12.0f); EXPECT_EQ(m_undo.GetHistorySize(), 1u);
    light.intensity = 18.0f;
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(owner.GetComponent<scene::TransformConstraintComponent>(), nullptr);
    EXPECT_EQ(World().FindByGuid(guid), nullptr); EXPECT_FLOAT_EQ(light.intensity, 18.0f);
    ASSERT_TRUE(m_undo.Redo());
    EXPECT_NE(owner.GetComponent<scene::TransformConstraintComponent>(), nullptr);
    EXPECT_NE(World().FindByGuid(guid), nullptr); EXPECT_FLOAT_EQ(light.intensity, 18.0f);
}

TEST_F(TargetPointReferenceOperatorTest, ExistingAnchorUndoPreservesOtherConstraintAndFlowFieldSettings)
{
    auto& owner = World().CreateGameObject("Flow"); auto& flow = owner.AddComponent<scene::FlowField>();
    flow.forces.emplace_back().strength = 4.0f;
    auto& constraint = owner.AddComponent<scene::TransformConstraintComponent>();
    constraint.positionOffset = {1.0f, 2.0f, 3.0f};
    auto& target = World().CreateGameObject("Anchor"); target.AddComponent<scene::TargetPointComponent>();
    editor::OpArgs args; args.Set("node", owner.instanceId); args.Set("kind", std::string("flow_field"));
    args.Set("target", target.instanceId); args.Set("mode", std::string("LookAt"));
    ASSERT_TRUE(Invoke("target_point.configure_anchor", args).ok);
    EXPECT_EQ(constraint.mode, scene::TransformConstraintMode::LookAt);
    constraint.weight = 0.4f; flow.forces[0].strength = 9.0f;
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(constraint.mode, scene::TransformConstraintMode::Parent);
    EXPECT_TRUE(constraint.target.guid.empty()); EXPECT_FLOAT_EQ(constraint.weight, 0.4f);
    EXPECT_FLOAT_EQ(flow.forces[0].strength, 9.0f);
    EXPECT_VEC3_NEAR(constraint.positionOffset, (math::Vector3{1.0f, 2.0f, 3.0f}), testkit::kTolerance);
}

TEST_F(TargetPointReferenceOperatorTest, AddedConstraintEditPreventsDestructiveUndo)
{
    auto& owner = World().CreateGameObject("Light"); owner.AddComponent<scene::LightComponent>();
    auto& target = World().CreateGameObject("Anchor"); target.AddComponent<scene::TargetPointComponent>();
    editor::OpArgs args; args.Set("node", owner.instanceId); args.Set("kind", std::string("light")); args.Set("target", target.instanceId);
    ASSERT_TRUE(Invoke("target_point.configure_anchor", args).ok);
    owner.GetComponent<scene::TransformConstraintComponent>()->weight = 0.4f;
    EXPECT_FALSE(m_undo.Undo());
    EXPECT_NE(owner.GetComponent<scene::TransformConstraintComponent>(), nullptr);
    EXPECT_NE(World().FindByGuid(target.instanceId), nullptr);
}

} /// @note namespace fbzz::tests
