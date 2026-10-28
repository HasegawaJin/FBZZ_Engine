/// @file    ManagedComponentServicesTests.cpp
/// @brief   runtime Component の実操作・scope拒否・recipe全件転送の回帰。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Components/MotionWarpComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/RagdollComponent.hpp>
#include <Engine/Scene/Components/SpringBoneComponent.hpp>
#include <Engine/Scene/Components/UIAnimator.hpp>
#include <Engine/Scene/Components/NavMeshPatrolComponent.hpp>
#include <Engine/Scene/Components/SplineComponents.hpp>
#include <Engine/Scene/Components/CameraRigComponents.hpp>
#include <Engine/Scene/Components/AudioSpatialComponents.hpp>
#include <Engine/Scene/Fields/FlowField.hpp>
#include "../../../Engine/src/Scene/Managed/ManagedComponentServices.hpp"
#include "../../../Engine/src/Scene/Managed/ManagedComponentTransfers.hpp"
#include "../../../Engine/src/Scene/Managed/ManagedServicesBridge.hpp"
#include <array>
#include <limits>
namespace fbzz::tests {
using ComponentStatus = scene::ManagedBridgeStatus;
using ComponentOp = scene::ManagedBridgeOp;
class ManagedComponentServicesTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp(); m_previousPlay = scene::Script::IsInPlayMode(); scene::Script::SetInPlayMode(true);
        m_object = &m_scene.CreateGameObject("Component services"); m_view.SetContext(&m_scene, m_object);
    }
    void TearDown() override
    {
        m_transfers.EndCall(13); m_view.SetContext(nullptr, nullptr); m_scene.Clear();
        scene::Script::SetInPlayMode(m_previousPlay); EngineFixture::TearDown();
    }
    ComponentStatus Run(ComponentOp op, uint32_t selector, bool read, int64_t integer = 0,
        std::span<const float> values = {}, std::span<const std::string> text = {}, bool fixed = false, scene::GameObject* related = nullptr)
    {
        scene::ManagedBridgeRequest request; request.op = op; request.index = selector; request.flags = read ? 1 : 0;
        return scene::managed_detail::RunComponentServices(m_scene, m_view, *m_object, related, request, integer, values, text, m_result, fixed);
    }
    ComponentStatus Names(uint32_t selector, int64_t integer, std::span<const float> values = {}, std::span<const std::string> text = {})
    {
        scene::ManagedBridgeRequest request; request.op = ComponentOp::SERVICE_RAGDOLL; request.index = selector;
        request.ownerId = 7; request.instanceId = 9; request.generation = 11; request.callId = 13;
        return m_transfers.Run(m_scene, *m_object, request, integer, values, text, m_result, true);
    }
    scene::Scene m_scene;
    scene::Script m_view;
    scene::GameObject* m_object = nullptr;
    scene::managed_detail::ManagedServiceResult m_result;
    scene::managed_detail::ManagedComponentTransfers m_transfers;
    bool m_previousPlay = false;
};
TEST_F(ManagedComponentServicesTest, MotionWarpNormalizesPoseAndRejectsDegenerateInputBeforeMutation)
{
    auto& warp = m_object->AddComponent<scene::MotionWarpComponent>();
    const std::array<float, 8> invalid{1, 2, 3, 0, 0, 0, 0, 0.5f}, valid{1, 2, 3, 0, 0, 0, 2, 0.5f};
    m_result.integer = 99;
    EXPECT_EQ(Run(ComponentOp::SERVICE_MOTION_WARP, 6, false, 0, invalid), ComponentStatus::INVALID_ARGUMENT);
    EXPECT_FALSE(warp.target.active); EXPECT_EQ(warp.runtimeWarpCount, 0U); EXPECT_EQ(m_result.integer, 99);
    ASSERT_EQ(Run(ComponentOp::SERVICE_MOTION_WARP, 6, false, 0, valid), ComponentStatus::OK);
    EXPECT_TRUE(warp.target.active); EXPECT_TRUE(warp.target.warpRotation); EXPECT_EQ(warp.runtimeWarpCount, 1U);
    EXPECT_VEC3_NEAR(warp.target.position, math::Vector3(1, 2, 3), testkit::kTolerance); EXPECT_NEAR(warp.target.rotation.w, 1, testkit::kTolerance);
    EXPECT_EQ(Run(ComponentOp::SERVICE_MOTION_WARP, 9, true), ComponentStatus::INVALID_ARGUMENT); EXPECT_TRUE(warp.target.active);
    ASSERT_EQ(Run(ComponentOp::SERVICE_MOTION_WARP, 9, false), ComponentStatus::OK); EXPECT_FALSE(warp.target.active);
}
TEST_F(ManagedComponentServicesTest, IkRetargetChecksSceneAndRetainsNamedChainAndClamping)
{
    auto& ik = m_object->AddComponent<scene::IKSolverComponent>(); ik.chains.emplace_back(); ik.chains[0].targetName = "Hand";
    auto& target = m_scene.CreateGameObject("Grip"); const std::array<std::string, 1> name{"Hand"};
    ASSERT_EQ(Run(ComponentOp::SERVICE_IK, 8, false, 0, {}, name, false, &target), ComponentStatus::OK);
    EXPECT_EQ(ik.chains[0].targetEntity, target.GetID());
    scene::Scene other; auto& foreign = other.CreateGameObject("Other grip");
    EXPECT_EQ(Run(ComponentOp::SERVICE_IK, 8, false, 0, {}, name, false, &foreign), ComponentStatus::INVALID_ARGUMENT);
    EXPECT_EQ(ik.chains[0].targetEntity, target.GetID());
    const std::array<float, 1> weight{2}; ASSERT_EQ(Run(ComponentOp::SERVICE_IK, 6, false, 0, weight, name), ComponentStatus::OK);
    EXPECT_NEAR(ik.chains[0].weight, 1, testkit::kTolerance);
    ASSERT_EQ(Run(ComponentOp::SERVICE_IK, 8, false, 0, {}, name), ComponentStatus::OK); EXPECT_EQ(ik.chains[0].targetEntity, scene::EntityID::INVALID);
}
TEST_F(ManagedComponentServicesTest, RagdollCommandsAreFixedOnlyAndPreserveNativePendingRequests)
{
    auto& ragdoll = m_object->AddComponent<scene::RagdollComponent>(); const std::array<float, 3> begin{2, 0.4f, 0};
    EXPECT_EQ(Run(ComponentOp::SERVICE_RAGDOLL, 13, false, 0, begin), ComponentStatus::INVALID_SCOPE); EXPECT_FALSE(ragdoll.beginRequested);
    ASSERT_EQ(Run(ComponentOp::SERVICE_RAGDOLL, 13, false, 0, begin, {}, true), ComponentStatus::OK);
    EXPECT_TRUE(ragdoll.beginRequested); EXPECT_FALSE(ragdoll.activeRequested); EXPECT_NEAR(ragdoll.activationWeight, 0.4f, testkit::kTolerance);
    const std::array<float, 7> impulse{1, 2, 3, 4, 5, 6, 2};
    ASSERT_EQ(Run(ComponentOp::SERVICE_RAGDOLL, 17, false, 0, impulse, {}, true), ComponentStatus::OK);
    ASSERT_EQ(ragdoll.pendingImpulses.size(), 1U); EXPECT_VEC3_NEAR(ragdoll.pendingImpulses[0].velocity, math::Vector3(4, 5, 6), testkit::kTolerance);
    scene::Script::SetInPlayMode(false);
    EXPECT_EQ(Run(ComponentOp::SERVICE_RAGDOLL, 15, false, 0, {}, {}, true), ComponentStatus::INVALID_SCOPE);
    EXPECT_FALSE(ragdoll.endRequested);
}
TEST_F(ManagedComponentServicesTest, SpringChainsReuseNativeCreationForceAndResetSemantics)
{
    auto& spring = m_object->AddComponent<scene::SpringBoneComponent>(); const std::array<std::string, 1> root{"Tail"};
    ASSERT_EQ(Run(ComponentOp::SERVICE_SPRING_BONE, 2, false, 3, {}, root), ComponentStatus::OK);
    ASSERT_EQ(spring.chains.size(), 1U); EXPECT_NEAR(spring.chains[0].weight, 0, testkit::kTolerance); EXPECT_EQ(spring.chains[0].maxDepth, 3);
    const std::array<float, 4> force{1, 2, 3, 4}; ASSERT_EQ(Run(ComponentOp::SERVICE_SPRING_BONE, 11, false, 0, force, root), ComponentStatus::OK);
    EXPECT_VEC3_NEAR(spring.chains[0].gravityDirection, math::Vector3(1, 2, 3), testkit::kTolerance); EXPECT_NEAR(spring.chains[0].gravityPower, 4, testkit::kTolerance);
    spring.hasLastOwnerPosition = true;
    ASSERT_EQ(Run(ComponentOp::SERVICE_SPRING_BONE, 13, false), ComponentStatus::OK); EXPECT_FALSE(spring.hasLastOwnerPosition);
    ASSERT_EQ(Run(ComponentOp::SERVICE_SPRING_BONE, 12, false, 0, {}, root), ComponentStatus::OK); EXPECT_NEAR(spring.chains[0].gravityPower, 0, testkit::kTolerance);
}
TEST_F(ManagedComponentServicesTest, UiTweensExposeRotationFillMaterialAndStopOneChannelInEdit)
{
    auto& ui = m_object->AddComponent<scene::UIAnimator>(); scene::Script::SetInPlayMode(false);
    const std::array<float, 3> tween{0, 1, 0.25f}; const std::array<std::string, 1> property{"Progress"};
    ASSERT_EQ(Run(ComponentOp::SERVICE_UI_ANIMATOR, 6, false, 14, tween), ComponentStatus::OK);
    ASSERT_EQ(Run(ComponentOp::SERVICE_UI_ANIMATOR, 7, false, 0, tween), ComponentStatus::OK);
    ASSERT_EQ(Run(ComponentOp::SERVICE_UI_ANIMATOR, 8, false, 0, tween, property), ComponentStatus::OK);
    EXPECT_TRUE(ui.rotationTween.loop); EXPECT_TRUE(ui.rotationTween.pingPong); EXPECT_EQ(ui.rotationTween.easing, scene::UIEasingType::EaseOut);
    EXPECT_TRUE(ui.fillTween.active); EXPECT_TRUE(ui.materialTween.active); EXPECT_EQ(ui.materialParam, "Progress");
    ASSERT_EQ(Run(ComponentOp::SERVICE_UI_ANIMATOR, 11, false, 3), ComponentStatus::OK);
    EXPECT_FALSE(ui.rotationTween.active); EXPECT_TRUE(ui.fillTween.active);
    EXPECT_EQ(Run(ComponentOp::SERVICE_UI_ANIMATOR, 8, false, 16, tween, property), ComponentStatus::INVALID_ARGUMENT);
}
TEST_F(ManagedComponentServicesTest, PatrolWaypointOutputsAndSplineRestartUseRealRows)
{
    auto& patrol = m_object->AddComponent<scene::NavMeshPatrolComponent>(); const std::array<float, 5> waypoint{2, 3, 4, 0.5f, 7};
    ASSERT_EQ(Run(ComponentOp::SERVICE_PATROL, 9, false, 0, waypoint), ComponentStatus::OK);
    ASSERT_EQ(patrol.waypoints.size(), 1U); EXPECT_VEC3_NEAR(patrol.waypoints[0], math::Vector3(2, 3, 4), testkit::kTolerance);
    EXPECT_EQ(Run(ComponentOp::SERVICE_PATROL, 12, true, 2), ComponentStatus::OK); EXPECT_EQ(m_result.integer, 0); EXPECT_TRUE(m_result.values.empty());
    auto& follower = m_object->AddComponent<scene::SplineFollowerComponent>(); follower.reverse = true;
    auto& curve = m_scene.CreateGameObject("Curve"); curve.AddComponent<scene::SplineComponent>();
    ASSERT_EQ(Run(ComponentOp::SERVICE_SPLINE_FOLLOWER, 16, false, 0, {}, {}, false, &curve), ComponentStatus::OK);
    EXPECT_EQ(follower.spline.id, curve.GetID());
    ASSERT_EQ(Run(ComponentOp::SERVICE_SPLINE_FOLLOWER, 7, false, 1), ComponentStatus::OK); EXPECT_NEAR(follower.normalizedPosition, 1, testkit::kTolerance);
    const std::array<float, 1> position{-2}; ASSERT_EQ(Run(ComponentOp::SERVICE_SPLINE_FOLLOWER, 6, false, 0, position), ComponentStatus::OK);
    EXPECT_NEAR(follower.normalizedPosition, 0, testkit::kTolerance);
}
TEST_F(ManagedComponentServicesTest, CameraControlsKeepExactPriorityAndNativeShakeClamp)
{
    auto& shake = m_object->AddComponent<scene::CameraShakeComponent>(); const std::array<float, 2> start{-1, 0.5f};
    ASSERT_EQ(Run(ComponentOp::SERVICE_CAMERA_SHAKE, 4, false, INT32_MIN, start), ComponentStatus::OK);
    EXPECT_EQ(shake.seed, INT32_MIN); EXPECT_NEAR(shake.amplitude, 0, testkit::kTolerance); EXPECT_TRUE(shake.playing);
    auto& camera = m_object->AddComponent<scene::VirtualCameraComponent>();
    ASSERT_EQ(Run(ComponentOp::SERVICE_VIRTUAL_CAMERA, 4, false, INT32_MAX), ComponentStatus::OK); EXPECT_EQ(camera.priority, INT32_MAX);
    const std::array<float, 1> fov{200}; ASSERT_EQ(Run(ComponentOp::SERVICE_VIRTUAL_CAMERA, 6, false, 0, fov), ComponentStatus::OK);
    EXPECT_NEAR(camera.fovY, 179, testkit::kTolerance);
    const std::array<float, 1> invalid{std::numeric_limits<float>::quiet_NaN()};
    EXPECT_EQ(Run(ComponentOp::SERVICE_VIRTUAL_CAMERA, 6, false, 0, invalid), ComponentStatus::INVALID_ARGUMENT); EXPECT_NEAR(camera.fovY, 179, testkit::kTolerance);
}
TEST_F(ManagedComponentServicesTest, AudioSendAndFlowChannelsPreserveAllBitsAndBakedReadIdentity)
{
    auto& audio = m_object->AddComponent<scene::AudioMixerSendComponent>(); const std::array<float, 1> level{2}; const std::array<std::string, 1> bus{"SE"};
    ASSERT_EQ(Run(ComponentOp::SERVICE_AUDIO_MIXER_SEND, 4, false, 0, level, bus), ComponentStatus::OK);
    EXPECT_EQ(audio.busName, "SE"); EXPECT_NEAR(audio.sendLevel, 1, testkit::kTolerance);
    auto& flow = m_object->AddComponent<scene::FlowField>(); flow.forces.emplace_back(); flow.forces[0].fieldType = scene::FlowFieldType::Baked;
    ASSERT_EQ(Run(ComponentOp::SERVICE_FLOW_FIELD, 2, true), ComponentStatus::OK); EXPECT_EQ(m_result.integer, 6);
    EXPECT_EQ(Run(ComponentOp::SERVICE_FLOW_FIELD, 3, false, 6), ComponentStatus::INVALID_ARGUMENT); EXPECT_EQ(flow.forces[0].fieldType, scene::FlowFieldType::Baked);
    ASSERT_EQ(Run(ComponentOp::SERVICE_FLOW_FIELD, 13, false, 0x80000001LL), ComponentStatus::OK);
    EXPECT_EQ(flow.forces[0].channels, 0x80000001U); EXPECT_EQ(flow.forces[1].channels, 0x80000001U);
    flow.forces.clear(); EXPECT_EQ(Run(ComponentOp::SERVICE_FLOW_FIELD, 0, true), ComponentStatus::NOT_FOUND);
    ASSERT_EQ(Run(ComponentOp::SERVICE_FLOW_FIELD, 16, true), ComponentStatus::OK); EXPECT_EQ(m_result.integer, 0); EXPECT_TRUE(flow.forces.empty());
}
TEST_F(ManagedComponentServicesTest, BoneRecipesStageBeyondPacketCountAndCommitOnlyOnce)
{
    auto& ragdoll = m_object->AddComponent<scene::RagdollComponent>(); ragdoll.rootBoneName = "Original"; ragdoll.maxDepth = 2;
    const std::array<float, 1> roots{0}, final{1}; const std::array<std::string, 1> count{"130"};
    ASSERT_EQ(Names(40, 4, roots, count), ComponentStatus::OK); const int64_t token = m_result.integer;
    for (int index = 0; index < 130; ++index) {
        const std::array<std::string, 3> chunk{std::to_string(index), "0", "Root" + std::to_string(index)};
        ASSERT_EQ(Names(41, token, final, chunk), ComponentStatus::OK);
        EXPECT_EQ(ragdoll.rootBoneName, "Original"); EXPECT_TRUE(ragdoll.extraRootBones.empty());
    }
    ASSERT_EQ(Names(42, token), ComponentStatus::OK); EXPECT_EQ(ragdoll.rootBoneName, "Root0");
    ASSERT_EQ(ragdoll.extraRootBones.size(), 129U); EXPECT_EQ(ragdoll.extraRootBones.back(), "Root129"); EXPECT_EQ(ragdoll.maxDepth, 4);
    const std::array<float, 1> exclusions{1}; const std::array<std::string, 1> one{"1"};
    ASSERT_EQ(Names(40, 0, exclusions, one), ComponentStatus::OK); const int64_t next = m_result.integer;
    const std::array<std::string, 3> wrongOffset{"0", "1", "Hand"};
    EXPECT_EQ(Names(41, next, final, wrongOffset), ComponentStatus::INVALID_ARGUMENT); EXPECT_TRUE(ragdoll.excludedRootBones.empty());
    ASSERT_EQ(Names(40, 0, exclusions, one), ComponentStatus::OK); const int64_t revision = m_result.integer;
    const std::array<std::string, 3> valid{"0", "0", "Hand"}; ASSERT_EQ(Names(41, revision, final, valid), ComponentStatus::OK);
    ragdoll.excludedRootBones = {"External"}; EXPECT_EQ(Names(42, revision), ComponentStatus::INVALID_SCOPE);
    ASSERT_EQ(ragdoll.excludedRootBones.size(), 1U); EXPECT_EQ(ragdoll.excludedRootBones[0], "External");
}
} /// @note namespace fbzz::tests
