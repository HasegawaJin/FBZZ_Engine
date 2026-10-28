/// @file    TransformSystemTests.cpp
/// @brief   描画補間の階層同期と Gameplay 姿勢処理の同期境界を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/VFXLineSystem.hpp>
#include <Engine/Scene/Systems/VFXBeamSystem.hpp>
#include <Engine/Scene/Systems/NavMeshPatrolSystem.hpp>
#include <Engine/Scene/Systems/NavMeshBakeSystem.hpp>
#include <Engine/Scene/Systems/LODSystem.hpp>
#include <Engine/Scene/Components/VFXBeamComponent.hpp>
#include <Engine/Scene/Components/TrailComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/LODGroupComponent.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/CameraRigComponents.hpp>
#include <Engine/Scene/Components/ConstraintComponents.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/RagdollComponent.hpp>
#include <Engine/Scene/Components/SplineComponents.hpp>
#include <Engine/Scene/Components/SpringBoneComponent.hpp>
#include <Engine/Scene/Systems/GameplayComponentSystems.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Math/MathUtils.hpp>
#include <Physics/World.hpp>
#include <memory>
#include <utility>

namespace fbzz::tests {
namespace {

/// @note 実デバイスを作らず、姿勢処理の資源可用性だけを通常の ResourceManager で表す。
class PoseCpuRenderer final : public renderer::IRenderer {
public:
    void Shutdown() override {}
    void BeginFrame() override {}
    void EndFrame() override {}
    void Clear(const math::Vector4&) override {}
    void Submit(const renderer::DrawCall&, renderer::ResourceManager&) override {}
    void Dispatch(const renderer::ComputeCall&, renderer::ResourceManager&) override {}
    void Resize(uint32_t, uint32_t) override {}
    uint32_t GetWidth() const override { return 1; }
    uint32_t GetHeight() const override { return 1; }
    void SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>, renderer::ResourceManager&) override {}
    void ClearDepth() override {}
    void SetViewport(uint32_t, uint32_t, uint32_t, uint32_t) override {}
    std::unique_ptr<renderer::IShader> CreateNativeShader(const std::string&) override { return {}; }
    std::unique_ptr<renderer::IConstantBuffer> CreateNativeConstantBuffer(size_t) override { return {}; }
    std::unique_ptr<renderer::IBuffer> CreateNativeVertexBuffer(const void*, size_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IBuffer> CreateNativeIndexBuffer(const void*, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture(const std::string&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromData(const uint8_t*, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture3DFromData(const uint8_t*, uint32_t, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromRenderTarget(renderer::IRenderTarget&, uint32_t, renderer::RenderTargetTextureKind) override { return {}; }
    std::unique_ptr<renderer::IPipelineState> CreateNativePipelineState(const renderer::PipelineStateDesc&) override { return {}; }
    std::unique_ptr<renderer::IRenderTarget> CreateNativeRenderTarget(uint32_t, uint32_t, const renderer::RenderTargetDesc&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeComputeTexture(uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeRWStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
};

class TransformSystemTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    physics::World m_world;

    SystemContext Context(bool simulating = true)
    {
        return {m_scene, m_world, nullptr, nullptr, 1.0f / 60.0f, 1.0f / 60.0f,
            simulating, simulating, 0.5f};
    }

    std::pair<scene::GameObject*, scene::GameObject*> InterpolatedHierarchy()
    {
        auto& root = m_scene.CreateGameObject("Body");
        auto& child = m_scene.CreateGameObject("Child");
        child.SetParent(root);
        root.transform.position = {10.0f, 0.0f, 0.0f};
        child.transform.position = {2.0f, 0.0f, 0.0f};
        child.SetActive(false);
        scene::RigidBodyComponent body;
        body.rigidBody = std::make_unique<physics::RigidBody>();
        body.rigidBody->SetPosition(root.transform.position);
        body.ResetPhysicsSyncState(math::Vector3::ZERO, math::Quaternion::Identity());
        body.CommitPhysicsSyncState(root.transform.position, math::Quaternion::Identity());
        root.AddComponent<scene::RigidBodyComponent>(std::move(body));
        auto context = Context();
        scene::TransformLateUpdate system;
        system.Update(context);
        return {&root, &child};
    }

    void ExpectInterpolatedHierarchy(const scene::GameObject& root, const scene::GameObject& child)
    {
        EXPECT_VEC3_NEAR(root.transform.worldPosition, (math::Vector3{5.0f, 0.0f, 0.0f}), testkit::kTolerance);
        EXPECT_VEC3_NEAR(child.transform.worldPosition, (math::Vector3{7.0f, 0.0f, 0.0f}), testkit::kTolerance);
        EXPECT_VEC3_NEAR(root.transform.position, (math::Vector3{10.0f, 0.0f, 0.0f}), testkit::kTolerance);
        EXPECT_VEC3_NEAR(child.transform.position, (math::Vector3{2.0f, 0.0f, 0.0f}), testkit::kTolerance);
    }
};

TEST_F(TransformSystemTest, LateUpdateInterpolatesWorldPoseWithoutChangingLocalOrPhysicsPose)
{
    auto [root, child] = InterpolatedHierarchy();
    auto& body = *root->GetComponent<scene::RigidBodyComponent>();
    const math::Quaternion quarterTurn = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(90.0f));
    root->transform.rotation = quarterTurn;
    root->transform.scale = {2.0f, 3.0f, 4.0f};
    child->transform.position = {0.0f, 0.0f, 1.0f};
    body.rigidBody->SetRotation(quarterTurn);
    body.lastPhysicsRotation = quarterTurn;
    auto& unrelated = m_scene.CreateGameObject("Unrelated");
    unrelated.transform.position = {20.0f, 0.0f, 0.0f};
    auto context = Context();

    scene::TransformLateUpdate system;
    system.Update(context);

    const math::Quaternion halfTurn = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(45.0f));
    EXPECT_QUAT_NEAR(root->transform.worldRotation, halfTurn, testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(root->transform.worldPosition, (math::Vector3{5.0f, 0.0f, 0.0f}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(child->transform.worldPosition, (math::Vector3{7.8284271f, 0.0f, 2.8284271f}), testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(child->transform.worldScale, (math::Vector3{2.0f, 3.0f, 4.0f}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(unrelated.transform.worldPosition, unrelated.transform.position, testkit::kTolerance);
    EXPECT_VEC3_NEAR(root->transform.position, (math::Vector3{10.0f, 0.0f, 0.0f}), testkit::kTolerance);
    EXPECT_QUAT_NEAR(root->transform.rotation, quarterTurn, testkit::kTolerance);
    EXPECT_VEC3_NEAR(body.rigidBody->GetPosition(), root->transform.position, testkit::kTolerance);
    EXPECT_QUAT_NEAR(body.rigidBody->GetRotation(), quarterTurn, testkit::kTolerance);

    scene::TransformPostPhysics postPhysics;
    postPhysics.Update(context);
    EXPECT_VEC3_NEAR(root->transform.worldPosition, root->transform.position, testkit::kTolerance);
    EXPECT_VEC3_NEAR(child->transform.worldPosition, (math::Vector3{14.0f, 0.0f, 0.0f}), testkit::kLooseTolerance);
}

TEST_F(TransformSystemTest, EmptyGameplayPoseSystemsPreserveTheInterpolatedHierarchy)
{
    auto [root, child] = InterpolatedHierarchy();
    auto& camera = m_scene.CreateGameObject("Camera");
    camera.AddComponent<scene::CameraComponent>().isMain = true;
    auto context = Context();
    scene::ConstraintSystem constraints;
    scene::SplineSystem spline;
    scene::CameraRigSystem rig;
    scene::BillboardSystem billboard;

    constraints.Update(context);
    ExpectInterpolatedHierarchy(*root, *child);
    spline.Update(context);
    ExpectInterpolatedHierarchy(*root, *child);
    rig.Update(context);
    ExpectInterpolatedHierarchy(*root, *child);
    billboard.Update(context);
    ExpectInterpolatedHierarchy(*root, *child);
}

TEST_F(TransformSystemTest, DisabledAndUnresolvedGameplayPoseSystemsPreserveTheInterpolatedHierarchy)
{
    auto [root, child] = InterpolatedHierarchy();
    auto& unused = m_scene.CreateGameObject("Unused");
    unused.AddComponent<scene::SocketAttachmentComponent>().enabled = false;
    unused.AddComponent<scene::TransformConstraintComponent>().enabled = false;
    unused.AddComponent<scene::SplineFollowerComponent>();
    unused.AddComponent<scene::CameraFollowComponent>();
    unused.AddComponent<scene::CameraBlendComponent>().playing = true;
    unused.AddComponent<scene::CameraShakeComponent>().enabled = false;
    unused.AddComponent<scene::BillboardComponent>().enabled = false;
    unused.AddComponent<scene::CameraComponent>().isMain = true;
    auto context = Context();
    scene::ConstraintSystem constraints;
    scene::SplineSystem spline;
    scene::CameraRigSystem rig;
    scene::BillboardSystem billboard;

    constraints.Update(context);
    ExpectInterpolatedHierarchy(*root, *child);
    spline.Update(context);
    ExpectInterpolatedHierarchy(*root, *child);
    rig.Update(context);
    ExpectInterpolatedHierarchy(*root, *child);
    billboard.Update(context);
    ExpectInterpolatedHierarchy(*root, *child);
}

TEST_F(TransformSystemTest, AnimatorBarrierRequiresResourcesAndAnActiveEnabledOwner)
{
    auto& owner = m_scene.CreateGameObject("Owner");
    auto& animator = owner.AddComponent<scene::AnimatorComponent>();
    auto& child = m_scene.CreateGameObject("NonBoneChild");
    child.SetParent(owner);
    child.transform.position = {2.0f, 0.0f, 0.0f};
    scene::FlushWorldTransforms(m_scene);
    scene::SetWorldPose(owner, {10.0f, 0.0f, 0.0f}, math::Quaternion::Identity(), math::Vector3::ONE);
    auto context = Context();
    scene::ConstraintSystem system;

    system.Update(context);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, (math::Vector3{2.0f, 0.0f, 0.0f}), testkit::kTolerance);

    PoseCpuRenderer cpuRenderer;
    renderer::ResourceManager resources(cpuRenderer);
    context.resources = &resources;
    system.Update(context);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, (math::Vector3{12.0f, 0.0f, 0.0f}), testkit::kTolerance);

    animator.enabled = false;
    scene::SetWorldPose(owner, {20.0f, 0.0f, 0.0f}, math::Quaternion::Identity(), math::Vector3::ONE);
    system.Update(context);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, (math::Vector3{12.0f, 0.0f, 0.0f}), testkit::kTolerance);

    animator.enabled = true;
    owner.SetActive(false);
    system.Update(context);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, (math::Vector3{12.0f, 0.0f, 0.0f}), testkit::kTolerance);

    owner.SetActive(true);
    system.Update(context);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, (math::Vector3{22.0f, 0.0f, 0.0f}), testkit::kTolerance);
}

TEST_F(TransformSystemTest, BonePoseProducerBarrierUpdatesNonBoneChildrenIncludingResourceFreeRagdoll)
{
    auto& owner = m_scene.CreateGameObject("Owner");
    auto& child = m_scene.CreateGameObject("NonBoneChild");
    child.SetParent(owner);
    child.transform.position = {2.0f, 0.0f, 0.0f};
    child.SetActive(false);
    scene::FlushWorldTransforms(m_scene);
    PoseCpuRenderer cpuRenderer;
    renderer::ResourceManager resources(cpuRenderer);
    auto context = Context();
    context.resources = &resources;
    scene::ConstraintSystem system;

    owner.AddComponent<scene::IKSolverComponent>();
    scene::SetWorldPose(owner, {10.0f, 0.0f, 0.0f}, math::Quaternion::Identity(), math::Vector3::ONE);
    system.Update(context);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, (math::Vector3{12.0f, 0.0f, 0.0f}), testkit::kTolerance);

    owner.RemoveComponent<scene::IKSolverComponent>();
    owner.AddComponent<scene::SpringBoneComponent>();
    scene::SetWorldPose(owner, {20.0f, 0.0f, 0.0f}, math::Quaternion::Identity(), math::Vector3::ONE);
    system.Update(context);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, (math::Vector3{22.0f, 0.0f, 0.0f}), testkit::kTolerance);

    owner.RemoveComponent<scene::SpringBoneComponent>();
    owner.AddComponent<scene::RagdollComponent>();
    context.resources = nullptr;
    scene::SetWorldPose(owner, {30.0f, 0.0f, 0.0f}, math::Quaternion::Identity(), math::Vector3::ONE);
    system.Update(context);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, (math::Vector3{32.0f, 0.0f, 0.0f}), testkit::kTolerance);
}

TEST_F(TransformSystemTest, TransformConstraintsObserveTheCompletedSocketAttachmentStage)
{
    auto& target = m_scene.CreateGameObject("Target");
    target.transform.position = {10.0f, 0.0f, 0.0f};
    auto& socket = m_scene.CreateGameObject("Socket");
    socket.SetParent(target);
    socket.transform.position = {3.0f, 0.0f, 0.0f};
    auto& attached = m_scene.CreateGameObject("Attached");
    auto& attachment = attached.AddComponent<scene::SocketAttachmentComponent>();
    attachment.target.id = target.GetID();
    attachment.socketName = socket.name;
    attachment.followRotation = false;
    auto& constrained = m_scene.CreateGameObject("Constrained");
    auto& constraint = constrained.AddComponent<scene::TransformConstraintComponent>();
    constraint.target.id = attached.GetID();
    constraint.mode = scene::TransformConstraintMode::Position;
    constraint.maintainOffset = false;
    auto& child = m_scene.CreateGameObject("Child");
    child.SetParent(constrained);
    child.transform.position = {2.0f, 0.0f, 0.0f};
    child.SetActive(false);
    auto context = Context();

    scene::ConstraintSystem system;
    system.Update(context);

    EXPECT_VEC3_NEAR(attached.transform.worldPosition, (math::Vector3{13.0f, 0.0f, 0.0f}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(constrained.transform.worldPosition, attached.transform.worldPosition, testkit::kTolerance);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, (math::Vector3{15.0f, 0.0f, 0.0f}), testkit::kTolerance);
}

TEST_F(TransformSystemTest, SplineFollowerSynchronizesItsInputsWithoutAConstraintPass)
{
    auto& splineParent = m_scene.CreateGameObject("SplineParent");
    splineParent.transform.position = {10.0f, 0.0f, 0.0f};
    splineParent.transform.scale = {2.0f, 2.0f, 2.0f};
    auto& splineObject = m_scene.CreateGameObject("Spline");
    splineObject.SetParent(splineParent);
    splineObject.transform.position = {1.0f, 0.0f, 0.0f};
    splineObject.AddComponent<scene::SplineComponent>().points = {{3.0f, 0.0f, 0.0f}, {5.0f, 0.0f, 0.0f}};
    auto& followerParent = m_scene.CreateGameObject("FollowerParent");
    followerParent.transform.position = {4.0f, 0.0f, 0.0f};
    followerParent.transform.scale = {2.0f, 2.0f, 2.0f};
    auto& followerObject = m_scene.CreateGameObject("Follower");
    followerObject.SetParent(followerParent);
    auto& follower = followerObject.AddComponent<scene::SplineFollowerComponent>();
    follower.spline.id = splineObject.GetID();
    follower.playing = false;
    follower.orientToPath = false;
    auto& child = m_scene.CreateGameObject("Child");
    child.SetParent(followerObject);
    child.transform.position = {1.0f, 0.0f, 0.0f};
    child.SetActive(false);
    auto context = Context();

    scene::SplineSystem system;
    system.Update(context);

    EXPECT_VEC3_NEAR(followerObject.transform.position, (math::Vector3{7.0f, 0.0f, 0.0f}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(followerObject.transform.worldPosition, (math::Vector3{18.0f, 0.0f, 0.0f}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, (math::Vector3{20.0f, 0.0f, 0.0f}), testkit::kTolerance);
}

TEST_F(TransformSystemTest, ConstraintPropagatesSmallPositionChangesToInactiveChildren)
{
    auto& target = m_scene.CreateGameObject("Target");
    target.transform.position = {0.0000005f, 0.0f, 0.0f};
    auto& constrained = m_scene.CreateGameObject("Constrained");
    auto& constraint = constrained.AddComponent<scene::TransformConstraintComponent>();
    constraint.target.id = target.GetID();
    constraint.mode = scene::TransformConstraintMode::Position;
    constraint.maintainOffset = false;
    auto& child = m_scene.CreateGameObject("Child");
    child.SetParent(constrained);
    child.SetActive(false);
    auto context = Context();

    scene::ConstraintSystem system;
    system.Update(context);

    EXPECT_VEC3_NEAR(constrained.transform.worldPosition, target.transform.position, 0.00000001f);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, target.transform.position, 0.00000001f);
}

TEST_F(TransformSystemTest, CameraFollowSynchronizesCurrentInputsAndInactiveDescendants)
{
    auto& targetParent = m_scene.CreateGameObject("TargetParent");
    targetParent.transform.position = {10.0f, 0.0f, 0.0f};
    auto& target = m_scene.CreateGameObject("Target");
    target.SetParent(targetParent);
    target.transform.position = {2.0f, 0.0f, 0.0f};
    auto& cameraParent = m_scene.CreateGameObject("CameraParent");
    cameraParent.transform.position = {4.0f, 0.0f, 0.0f};
    cameraParent.transform.scale = {2.0f, 2.0f, 2.0f};
    auto& camera = m_scene.CreateGameObject("Camera");
    camera.SetParent(cameraParent);
    auto& follow = camera.AddComponent<scene::CameraFollowComponent>();
    follow.target.id = target.GetID();
    follow.offset = math::Vector3::ZERO;
    follow.positionDamping = 0.0f;
    follow.lookAtTarget = false;
    auto& child = m_scene.CreateGameObject("Child");
    child.SetParent(camera);
    child.transform.position = {1.0f, 0.0f, 0.0f};
    child.SetActive(false);
    auto context = Context();

    scene::CameraRigSystem system;
    system.Update(context);

    EXPECT_VEC3_NEAR(camera.transform.position, (math::Vector3{4.0f, 0.0f, 0.0f}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(camera.transform.worldPosition, (math::Vector3{12.0f, 0.0f, 0.0f}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, (math::Vector3{14.0f, 0.0f, 0.0f}), testkit::kTolerance);
}

TEST_F(TransformSystemTest, DisablingCameraShakeStillRemovesItsPreviouslyAppliedPose)
{
    auto& camera = m_scene.CreateGameObject("Camera");
    camera.transform.position = {7.0f, 0.0f, 0.0f};
    const math::Quaternion quarterTurn = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(90.0f));
    camera.transform.rotation = quarterTurn;
    auto& shake = camera.AddComponent<scene::CameraShakeComponent>();
    shake.enabled = false;
    shake.runtimeInitialized = true;
    shake.appliedPositionOffset = {2.0f, 0.0f, 0.0f};
    shake.appliedRotationOffset = quarterTurn;
    auto& child = m_scene.CreateGameObject("Child");
    child.SetParent(camera);
    child.transform.position = {1.0f, 0.0f, 0.0f};
    child.SetActive(false);
    auto context = Context();

    scene::CameraRigSystem system;
    system.Update(context);

    EXPECT_VEC3_NEAR(camera.transform.position, (math::Vector3{5.0f, 0.0f, 0.0f}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(camera.transform.worldPosition, camera.transform.position, testkit::kTolerance);
    EXPECT_QUAT_NEAR(camera.transform.worldRotation, math::Quaternion::Identity(), testkit::kTolerance);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, (math::Vector3{6.0f, 0.0f, 0.0f}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(shake.appliedPositionOffset, math::Vector3::ZERO, testkit::kTolerance);
    EXPECT_QUAT_NEAR(shake.appliedRotationOffset, math::Quaternion::Identity(), testkit::kTolerance);
}

TEST_F(TransformSystemTest, BillboardUsesTheCurrentCameraPoseAndSynchronizesInactiveDescendants)
{
    auto& camera = m_scene.CreateGameObject("Camera");
    camera.AddComponent<scene::CameraComponent>().isMain = true;
    const math::Quaternion quarterTurn = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(90.0f));
    camera.transform.rotation = quarterTurn;
    auto& billboard = m_scene.CreateGameObject("Billboard");
    billboard.AddComponent<scene::BillboardComponent>().mode = scene::BillboardMode::MatchCamera;
    auto& child = m_scene.CreateGameObject("Child");
    child.SetParent(billboard);
    child.transform.position = {0.0f, 0.0f, 2.0f};
    child.SetActive(false);
    auto context = Context();

    scene::BillboardSystem system;
    system.Update(context);

    EXPECT_QUAT_NEAR(billboard.transform.worldRotation, quarterTurn, testkit::kTolerance);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, (math::Vector3{2.0f, 0.0f, 0.0f}), testkit::kLooseTolerance);
}

TEST_F(TransformSystemTest, PrePhysicsInitializesChunkedBodiesAndPreservesExistingSyncState)
{
    for (int index = 0; index < 129; ++index) {
        auto& object = m_scene.CreateGameObject("Body");
        object.transform.position = {static_cast<float>(index + 10), 0.0f, 0.0f};
        scene::RigidBodyComponent body;
        body.rigidBody = std::make_unique<physics::RigidBody>();
        if (index == 0) body.enabled = false;
        if (index == 64) {
            body.rigidBody->SetPosition({99.0f, 0.0f, 0.0f});
            body.ResetPhysicsSyncState({99.0f, 0.0f, 0.0f}, math::Quaternion::Identity());
        }
        object.AddComponent<scene::RigidBodyComponent>(std::move(body));
    }
    auto context = Context();

    scene::TransformPrePhysics system;
    system.Update(context);

    int index = 0;
    for (auto& object : m_scene.GameObjects()) {
        const auto& body = *object.GetComponent<scene::RigidBodyComponent>();
        const math::Vector3 expected{index == 0 ? 0.0f : index == 64 ? 99.0f : static_cast<float>(index + 10), 0.0f, 0.0f};
        EXPECT_VEC3_NEAR(body.rigidBody->GetPosition(), expected, testkit::kTolerance);
        EXPECT_EQ(body.hasPhysicsSyncState, index != 0);
        if (index != 0) EXPECT_VEC3_NEAR(body.lastPhysicsPosition, expected, testkit::kTolerance);
        ++index;
    }
}

TEST_F(TransformSystemTest, ReusesTraversalStorageAcrossWideHierarchiesAndSceneReplacement)
{
    auto& root = m_scene.CreateGameObject("Wide root");
    root.transform.position = {10, 0, 0};
    for (int index = 0; index < 512; ++index) {
        auto& child = m_scene.CreateGameObject("Child");
        child.SetParent(root);
        child.transform.position = {static_cast<float>(index), 0, 0};
        child.SetActive(false);
    }
    scene::FlushWorldTransforms(m_scene);
    for (int index = 0; index < root.GetChildCount(); ++index)
        EXPECT_FLOAT_EQ(root.GetChild(index)->transform.worldPosition.x, 10.0f + index);
    root.transform.position.x = 20;
    scene::FlushWorldTransforms(root);
    for (int index = 0; index < root.GetChildCount(); ++index)
        EXPECT_FLOAT_EQ(root.GetChild(index)->transform.worldPosition.x, 20.0f + index);
    m_scene.Clear();
    auto& replacement = m_scene.CreateGameObject("Replacement");
    replacement.transform.position = {-3, 2, 1};
    scene::FlushWorldTransforms(replacement);
    scene::FlushWorldTransforms(m_scene);
    EXPECT_VEC3_NEAR(replacement.transform.worldPosition, replacement.transform.position, testkit::kTolerance);
}

} /// @note namespace
TEST_F(TransformSystemTest, EmptyOptionalConsumersPreserveInterpolationAndResetRendererVisibility)
{
    auto [root, child] = InterpolatedHierarchy();
    auto& mesh = root->AddComponent<scene::MeshRenderer>();
    mesh.enabled = false;
    mesh.lodVisible = false;
    mesh.lodDither = 0.75f;
    auto& skinned = child->AddComponent<scene::SkinnedMeshRenderer>();
    skinned.enabled = false;
    skinned.lodVisible = false;
    skinned.lodDither = -0.25f;
    auto context = Context();

    scene::LODSystem{}.Update(context);
    ExpectInterpolatedHierarchy(*root, *child);
    EXPECT_TRUE(mesh.lodVisible);
    EXPECT_FLOAT_EQ(mesh.lodDither, 0.0f);
    EXPECT_FALSE(mesh.enabled);
    EXPECT_TRUE(skinned.lodVisible);
    EXPECT_FLOAT_EQ(skinned.lodDither, 0.0f);
    EXPECT_FALSE(skinned.enabled);

    scene::VFXBeamSystem{}.Update(context);
    ExpectInterpolatedHierarchy(*root, *child);
    scene::VFXLineSystem{}.Update(context);
    ExpectInterpolatedHierarchy(*root, *child);
    scene::NavMeshPatrolSystem{}.Update(context);
    ExpectInterpolatedHierarchy(*root, *child);
    scene::NavMeshBakeSystem{}.Update(context);
    ExpectInterpolatedHierarchy(*root, *child);
}

TEST_F(TransformSystemTest, BeamAddedAfterAnEmptyUpdateSynchronizesTheCurrentEndpoints)
{
    auto context = Context();
    scene::VFXBeamSystem system;
    system.Update(context);
    auto& source = m_scene.CreateGameObject("Source");
    auto& target = m_scene.CreateGameObject("Target");
    source.transform.position = {7.0f, 2.0f, 0.0f};
    target.transform.position = {20.0f, 4.0f, 0.0f};
    auto& owner = m_scene.CreateGameObject("Beam");
    auto& trail = owner.AddComponent<scene::TrailComponent>();
    auto& beam = owner.AddComponent<scene::VFXBeamComponent>();
    beam.segments = 1;
    beam.fromEntity = {source.GetID(), source.instanceId, m_scene.GetRenderSceneGeneration()};
    beam.toEntity = {target.GetID(), target.instanceId, m_scene.GetRenderSceneGeneration()};

    system.Update(context);

    ASSERT_EQ(trail.beamPoints.size(), 2u);
    EXPECT_VEC3_NEAR(trail.beamPoints.front(), source.transform.position, testkit::kTolerance);
    EXPECT_VEC3_NEAR(trail.beamPoints.back(), target.transform.position, testkit::kTolerance);
}


TEST_F(TransformSystemTest, ParentFirstPropagationIgnoresFlatStorageOrderAndTracksHierarchyEdits)
{
    auto& leaf = m_scene.CreateGameObject("Leaf");
    auto& child = m_scene.CreateGameObject("Child");
    auto& root = m_scene.CreateGameObject("Root");
    child.SetParent(root);
    leaf.SetParent(child);
    leaf.SetActive(false);
    root.transform.position = {5, 0, 0};
    child.transform.position = {2, 0, 0};
    leaf.transform.position = {1, 0, 0};
    scene::FlushWorldTransforms(m_scene);
    EXPECT_FLOAT_EQ(leaf.transform.worldPosition.x, 8.0f);
    auto context = Context();
    root.transform.position.x = 9;
    scene::TransformLateUpdate{}.Update(context);
    EXPECT_FLOAT_EQ(leaf.transform.worldPosition.x, 12.0f);
    leaf.SetParent(root);
    scene::FlushWorldTransforms(m_scene);
    EXPECT_FLOAT_EQ(leaf.transform.worldPosition.x, 10.0f);
    leaf.ClearParent();
    scene::FlushWorldTransforms(m_scene);
    EXPECT_FLOAT_EQ(leaf.transform.worldPosition.x, 1.0f);
    m_scene.Clear();
    auto& replacement = m_scene.CreateGameObject("Replacement");
    replacement.transform.position = {-7, 0, 0};
    scene::FlushWorldTransforms(m_scene);
    EXPECT_FLOAT_EQ(replacement.transform.worldPosition.x, -7.0f);
}

} /// @note namespace fbzz::tests
