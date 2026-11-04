/// @file    ManagedComponentServicesRuntimeTests.cpp
/// @brief   実 CoreCLR ABI を通じ runtime11型と粒子の状態・拒否・row寿命を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <Engine/Scene/ScriptComponent.hpp>
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/Managed/ManagedScriptAdapter.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Scene/Components/MotionWarpComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/RagdollComponent.hpp>
#include <Engine/Scene/Components/SpringBoneComponent.hpp>
#include <Engine/Scene/Components/UIAnimator.hpp>
#include <Engine/Scene/Components/NavMeshPatrolComponent.hpp>
#include <Engine/Scene/Components/SplineComponents.hpp>
#include <Engine/Scene/Components/CameraRigComponents.hpp>
#include <Engine/Scene/Components/AudioSpatialComponents.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Fields/FlowField.hpp>
#include "../../../Engine/src/Scene/Managed/ManagedObjectControls.hpp"
#include <string>
#include <string_view>
namespace fbzz::tests {
class ManagedComponentServicesRuntimeTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    scene::GameObject* m_owner = nullptr;
    scene::GameObject* m_related = nullptr;
    scene::GameObject* m_spline = nullptr;
    scene::ManagedScriptAdapter* m_adapter = nullptr;
    bool m_previousPlay = false;
    static void AddRows(scene::GameObject& object)
    {
        object.AddComponent<scene::MotionWarpComponent>();
        auto& ik = object.AddComponent<scene::IKSolverComponent>(); ik.chains.emplace_back(); ik.chains.back().targetName = "Hand";
        object.AddComponent<scene::RagdollComponent>(); object.AddComponent<scene::SpringBoneComponent>(); object.AddComponent<scene::UIAnimator>();
        object.AddComponent<scene::NavMeshPatrolComponent>(); object.AddComponent<scene::SplineFollowerComponent>();
        object.AddComponent<scene::CameraShakeComponent>(); object.AddComponent<scene::VirtualCameraComponent>();
        object.AddComponent<scene::AudioMixerSendComponent>(); object.AddComponent<scene::FlowField>().forces.emplace_back();
        object.AddComponent<scene::ParticleEmitter>();
    }
    void SetUp() override
    {
        EngineFixture::SetUp(); m_previousPlay = scene::Script::IsInPlayMode(); scene::Script::SetInPlayMode(true);
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        ASSERT_TRUE(runtime.Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY)) << runtime.LastDiagnostic();
        ASSERT_TRUE(runtime.Load(FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
        m_owner = &m_scene.CreateGameObject("CLR runtime components"); AddRows(*m_owner);
        m_related = &m_scene.CreateGameObject("Grip"); m_spline = &m_scene.CreateGameObject("Curve"); m_spline->AddComponent<scene::SplineComponent>();
        auto& entry = m_owner->AddComponent<scene::ScriptComponent>().scripts.emplace_back();
        entry.script = scene::ScriptFactory::Create("cs-internal:ManagedFixtures.RuntimeComponentsComponent", entry.savedScriptId);
        ASSERT_NE(entry.script, nullptr); entry.script->SetContext(&m_scene, m_owner);
        m_adapter = runtime.FindAdapter(entry.script.get()); ASSERT_NE(m_adapter, nullptr); ASSERT_TRUE(m_adapter->RefreshOwnerBinding());
        ASSERT_TRUE(m_adapter->ApplySettingsJson("{\"related\":\"" + m_related->instanceId + "\",\"splineObject\":\"" + m_spline->instanceId + "\"}"));
    }
    void TearDown() override
    {
        auto& runtime = scene::ManagedScriptRuntime::Instance(); runtime.Unload(&m_scene); m_scene.Clear();
        m_owner = m_related = m_spline = nullptr; m_adapter = nullptr;
        EXPECT_TRUE(runtime.Shutdown()) << runtime.LastDiagnostic(); scene::Script::SetInPlayMode(m_previousPlay); EngineFixture::TearDown();
    }
    bool Call(int operation, bool fixed = false)
    {
        if (!m_adapter->ApplySettingsJson("{\"operation\":" + std::to_string(operation) + "}")) return false;
        return fixed ? m_adapter->ExecuteProfiledCallback(&scene::Script::OnFixedUpdate, scene::ScriptCallbackKind::FIXED_UPDATE, "OnFixedUpdate")
            : m_adapter->ExecuteProfiledCallback(&scene::Script::OnUpdate, scene::ScriptCallbackKind::UPDATE, "OnUpdate");
    }
    scene::ManagedBridgeReply Field(std::string_view name)
    {
        scene::ManagedBridgeRequest request; request.op = scene::ManagedBridgeOp::READ_FIELD;
        request.generation = m_adapter->Generation(); request.instanceId = m_adapter->InstanceId(); request.ownerId = m_adapter->OwnerId();
        request.input = name.data(); request.inputBytes = static_cast<uint32_t>(name.size());
        scene::ManagedBridgeReply reply; EXPECT_TRUE(scene::ManagedScriptRuntime::Instance().Dispatch(request, reply)); return reply;
    }
};
TEST_F(ManagedComponentServicesRuntimeTest, ActualClrRuntimeFacadesChangeRowsAndReturnQueries)
{
    ASSERT_TRUE(Call(1)) << m_adapter->FaultDiagnostic();
    EXPECT_EQ(Field("connectionMask").integer, 2047); EXPECT_FLOAT_EQ(Field("observedWarpTime").x, 0.75f);
    EXPECT_FLOAT_EQ(Field("observedIKWeight").x, 1); EXPECT_FLOAT_EQ(Field("observedSpringWeight").x, 0.6f);
    EXPECT_NE(Field("observedUIPlaying").integer, 0); EXPECT_EQ(Field("observedWaypointCount").integer, 2);
    const auto waypoint = Field("observedWaypoint"); EXPECT_VEC3_NEAR((math::Vector3{waypoint.x, waypoint.y, waypoint.z}), (math::Vector3{5, 6, 7}), testkit::kTolerance);
    EXPECT_FLOAT_EQ(Field("observedSplinePosition").x, 0.4f); EXPECT_EQ(Field("observedShakeSeed").integer, INT32_MIN);
    EXPECT_EQ(Field("observedCameraPriority").integer, INT32_MAX); EXPECT_FLOAT_EQ(Field("observedAudioLevel").x, 1);
    EXPECT_EQ(Field("observedFlowChannels").integer, static_cast<int64_t>(INT32_MIN) + 1);
    const auto* warp = m_owner->GetComponent<scene::MotionWarpComponent>(); ASSERT_NE(warp, nullptr);
    EXPECT_TRUE(warp->target.active); EXPECT_TRUE(warp->target.warpRotation); EXPECT_FLOAT_EQ(warp->target.rotation.w, 1); EXPECT_FLOAT_EQ(warp->target.maxSpeed, 4);
    const auto* ik = m_owner->GetComponent<scene::IKSolverComponent>(); ASSERT_NE(ik, nullptr);
    ASSERT_EQ(ik->chains.size(), 1U); EXPECT_FALSE(ik->chains[0].enabled); EXPECT_EQ(ik->chains[0].targetEntity, m_related->GetID());
    const auto* spring = m_owner->GetComponent<scene::SpringBoneComponent>(); ASSERT_NE(spring, nullptr); ASSERT_EQ(spring->chains.size(), 1U);
    EXPECT_EQ(spring->chains[0].rootBoneName, "Tail"); EXPECT_EQ(spring->chains[0].maxDepth, 3); EXPECT_FLOAT_EQ(spring->chains[0].gravityPower, 4);
    const auto* ui = m_owner->GetComponent<scene::UIAnimator>(); ASSERT_NE(ui, nullptr);
    EXPECT_EQ(ui->positionTween.easing, scene::UIEasingType::EaseOut); EXPECT_TRUE(ui->positionTween.loop); EXPECT_TRUE(ui->positionTween.pingPong);
    EXPECT_TRUE(ui->rotationTween.active); EXPECT_TRUE(ui->fillTween.active); EXPECT_TRUE(ui->materialTween.active); EXPECT_EQ(ui->materialParam, "Progress");
    const auto* patrol = m_owner->GetComponent<scene::NavMeshPatrolComponent>(); ASSERT_NE(patrol, nullptr);
    EXPECT_EQ(patrol->mode, scene::NavMeshPatrolComponent::Mode::PING_PONG); EXPECT_EQ(patrol->currentIndex, 1U); EXPECT_FLOAT_EQ(patrol->waitTime, 0.25f);
    const auto* spline = m_owner->GetComponent<scene::SplineFollowerComponent>(); ASSERT_NE(spline, nullptr);
    EXPECT_EQ(spline->spline.id, m_spline->GetID()); EXPECT_TRUE(spline->playing); EXPECT_TRUE(spline->reverse); EXPECT_FALSE(spline->orientToPath); EXPECT_FLOAT_EQ(spline->speed, 2.5f);
    const auto* camera = m_owner->GetComponent<scene::VirtualCameraComponent>(); ASSERT_NE(camera, nullptr); EXPECT_FLOAT_EQ(camera->fovY, 179); EXPECT_FLOAT_EQ(camera->blendInTime, 0);
    const auto* shake = m_owner->GetComponent<scene::CameraShakeComponent>(); ASSERT_NE(shake, nullptr); EXPECT_TRUE(shake->playing); EXPECT_FLOAT_EQ(shake->frequency, 12);
    const auto* audio = m_owner->GetComponent<scene::AudioMixerSendComponent>(); ASSERT_NE(audio, nullptr); EXPECT_EQ(audio->busName, "SE"); EXPECT_FLOAT_EQ(audio->sendLevel, 1);
    const auto* flow = m_owner->GetComponent<scene::FlowField>(); ASSERT_NE(flow, nullptr); ASSERT_EQ(flow->forces.size(), 2U);
    EXPECT_EQ(flow->forces[0].fieldType, scene::FlowFieldType::Vortex); EXPECT_FLOAT_EQ(flow->forces[0].strength, 4.5f);
    EXPECT_FLOAT_EQ(flow->forces[0].falloffPower, 0.01f); EXPECT_EQ(flow->forces[0].channels, 0x80000001U); EXPECT_EQ(flow->forces[1].channels, 0x80000001U);
    ASSERT_TRUE(Call(1, true)) << m_adapter->FaultDiagnostic();
    const auto* ragdoll = m_owner->GetComponent<scene::RagdollComponent>(); ASSERT_NE(ragdoll, nullptr);
    EXPECT_NE(Field("fixedRequested").integer, 0); EXPECT_EQ(Field("observedRootCount").integer, 130);
    EXPECT_EQ(ragdoll->rootBoneName, "Bone0"); ASSERT_EQ(ragdoll->extraRootBones.size(), 129U); EXPECT_EQ(ragdoll->extraRootBones.back(), "Bone129");
    EXPECT_EQ(ragdoll->maxDepth, 3); EXPECT_EQ(ragdoll->profile, scene::RagdollProfileKind::Humanoid); EXPECT_TRUE(ragdoll->beginRequested);
    EXPECT_FLOAT_EQ(ragdoll->activationWeight, 0.6f); EXPECT_FLOAT_EQ(ragdoll->activationGravity, 0); ASSERT_EQ(ragdoll->pendingImpulses.size(), 1U);
    EXPECT_FALSE(m_adapter->IsRuntimeFaulted());
}
TEST_F(ManagedComponentServicesRuntimeTest, ActualClrWrongPhaseRagdollRequestLeavesRowUntouched)
{
    auto* ragdoll = m_owner->GetComponent<scene::RagdollComponent>(); ASSERT_NE(ragdoll, nullptr);
    const auto birth = m_scene.GetComponentLifetime<scene::RagdollComponent>(m_owner->GetID());
    ASSERT_TRUE(Call(2)) << m_adapter->FaultDiagnostic();
    EXPECT_NE(Field("wrongPhaseRejected").integer, 0); EXPECT_FALSE(ragdoll->beginRequested);
    EXPECT_FLOAT_EQ(ragdoll->activationWeight, 1); EXPECT_FLOAT_EQ(ragdoll->activationGravity, 1);
    EXPECT_EQ(m_scene.GetComponentLifetime<scene::RagdollComponent>(m_owner->GetID()), birth); EXPECT_FALSE(m_adapter->IsRuntimeFaulted());
}
TEST_F(ManagedComponentServicesRuntimeTest, ActualClrRelatedObjectRejectsForeignSceneWithoutClearingExistingIkTarget)
{
    ASSERT_TRUE(Call(3)) << m_adapter->FaultDiagnostic();
    auto* ik = m_owner->GetComponent<scene::IKSolverComponent>(); ASSERT_NE(ik, nullptr); ASSERT_EQ(ik->chains.size(), 1U);
    EXPECT_EQ(ik->chains[0].targetEntity, m_related->GetID());
    EXPECT_NE(Field("parentChanged").integer, 0); EXPECT_EQ(m_owner->GetParent(), m_related);
    scene::Scene foreignScene; auto& foreign = foreignScene.CreateGameObject("Foreign grip");
    ASSERT_TRUE(m_adapter->ApplySettingsJson("{\"related\":\"" + foreign.instanceId + "\"}"));
    ASSERT_TRUE(Call(3)) << m_adapter->FaultDiagnostic();
    EXPECT_NE(Field("relatedRejected").integer, 0); EXPECT_EQ(ik->chains[0].targetEntity, m_related->GetID());
    EXPECT_NE(Field("parentRejected").integer, 0); EXPECT_EQ(m_owner->GetParent(), m_related); EXPECT_FALSE(m_adapter->IsRuntimeFaulted());
}
TEST_F(ManagedComponentServicesRuntimeTest, ActualClrCachedFlowAndSavedReferenceDoNotReattachAfterRemoval)
{
    ASSERT_TRUE(Call(1)) << m_adapter->FaultDiagnostic();
    const auto original = m_scene.GetComponentLifetime<scene::FlowField>(m_owner->GetID());
    ASSERT_TRUE(Call(4)); EXPECT_EQ(Field("cachedIsAlive").integer, 0); EXPECT_NE(Field("savedReferenceMissing").integer, 0);
    scene::managed_detail::ManagedObjectControls::FlushScriptRemovals(m_scene); ASSERT_EQ(m_owner->GetComponent<scene::FlowField>(), nullptr);
    ASSERT_TRUE(Call(5)) << m_adapter->FaultDiagnostic();
    EXPECT_EQ(Field("cachedIsAlive").integer, 0); EXPECT_NE(Field("savedReferenceMissing").integer, 0); EXPECT_NE(Field("staleWriteRejected").integer, 0);
    EXPECT_NE(m_scene.GetComponentLifetime<scene::FlowField>(m_owner->GetID()), original); EXPECT_FLOAT_EQ(Field("observedFlowSpeed").x, 5);
    const auto* flow = m_owner->GetComponent<scene::FlowField>(); ASSERT_NE(flow, nullptr); ASSERT_EQ(flow->forces.size(), 1U); EXPECT_FLOAT_EQ(flow->forces[0].strength, 5);
}
TEST_F(ManagedComponentServicesRuntimeTest, ActualClrParticleSettingsGpuQueriesAndRejectedAssetsReachNativeEmitter)
{
    auto* particle = m_owner->GetComponent<scene::ParticleEmitter>(); ASSERT_NE(particle, nullptr);
    particle->settings.materialPath = "Preserved.mat"; particle->runtime.gpuInitialized = true; particle->runtime.gpuCapacity = 300;
    ASSERT_TRUE(Call(6)) << m_adapter->FaultDiagnostic();
    EXPECT_EQ(Field("observedParticleCapacity").integer, 23); EXPECT_FLOAT_EQ(Field("observedParticleDuration").x, 2.5f);
    EXPECT_EQ(Field("observedParticleRequested").integer, 1); EXPECT_NE(Field("observedParticleActual").integer, 0);
    EXPECT_EQ(Field("observedParticleFallback").integer, 0); EXPECT_FLOAT_EQ(Field("observedParticleNoise").x, 2); EXPECT_NE(Field("invalidMaterialRejected").integer, 0);
    const auto position = Field("observedParticlePosition"); EXPECT_VEC3_NEAR((math::Vector3{position.x, position.y, position.z}), (math::Vector3{1, 2, 3}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(particle->settings.emitVelocity, (math::Vector3{4, 5, 6}), testkit::kTolerance);
    EXPECT_FLOAT_EQ(particle->settings.velocitySpread, 0); EXPECT_FLOAT_EQ(particle->settings.sizeStart, 0.2f); EXPECT_FLOAT_EQ(particle->settings.sizeEnd, 0.4f);
    EXPECT_FALSE(particle->runtime.gpuInitialized); EXPECT_EQ(particle->runtime.gpuCapacity, 0); EXPECT_TRUE(particle->runtime.gpuClearPending);
    EXPECT_FALSE(particle->settings.loop); EXPECT_TRUE(particle->settings.clearOnStop); EXPECT_EQ(particle->settings.shape, scene::ParticleEmitterShape::Cone);
    EXPECT_EQ(particle->settings.renderMode, scene::ParticleRenderMode::StretchedBillboard); EXPECT_FLOAT_EQ(particle->settings.stretchedVelocityScale, 3);
    EXPECT_EQ(particle->settings.collisionMode, scene::ParticleCollisionMode::Depth); EXPECT_EQ(particle->settings.collisionResponse, scene::ParticleCollisionResponse::Kill);
    EXPECT_FLOAT_EQ(particle->settings.flowCoupling, 7); EXPECT_EQ(particle->settings.flowFieldChannels, 0x80000001U); EXPECT_FALSE(particle->settings.receiveFlowFields);
    EXPECT_EQ(particle->settings.materialPath, "Preserved.mat"); ASSERT_EQ(particle->settings.localForces.size(), 1U);
    EXPECT_EQ(particle->settings.localForces[0].fieldType, scene::FlowFieldType::Curl); EXPECT_FLOAT_EQ(particle->settings.localForces[0].noiseSpeed, -0.3f);
    ASSERT_TRUE(Call(7)) << m_adapter->FaultDiagnostic();
    EXPECT_EQ(Field("invalidParticleRejected").integer, 7); EXPECT_EQ(particle->settings.simulationMode, scene::ParticleSimulationMode::Gpu);
    EXPECT_EQ(particle->settings.collisionResponse, scene::ParticleCollisionResponse::Kill); EXPECT_VEC3_NEAR(particle->settings.emitVelocity, (math::Vector3{4, 5, 6}), testkit::kTolerance);
}
} /// @note namespace fbzz::tests
