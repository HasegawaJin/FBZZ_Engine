/// @file    RagdollSystemTests.cpp
/// @brief   描画デバイスに依存せず起動待ち・時計・復帰・最終姿勢の境界を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-13
#include <Engine/Scene/Components/RagdollComponent.hpp>
#include <Engine/Scene/Ragdoll/RagdollControls.hpp>
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/ComponentReflectionCodec.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Ragdoll/RagdollPlayback.hpp>
#include <Engine/Scene/SkinnedPoseBounds.hpp>
#include <Engine/Scene/Systems/RagdollSystem.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include "../../../Engine/src/Scene/Managed/ManagedObjectControls.hpp"
#include <Physics/OBBCollider.hpp>
#include <Physics/World.hpp>
#include <Math/MathUtils.hpp>
#include <cmath>
#include <limits>

namespace fbzz::tests {

namespace {
}

class RagdollSystemTest : public testkit::EngineFixture {
protected:
    asset::Model m_model;
    physics::World m_world;
    scene::Scene m_scene;
    scene::EntityID m_actor = scene::EntityID::INVALID;

    void SetUp() override
    {
        testkit::EngineFixture::SetUp();
        m_actor = m_scene.CreateGameObject("Actor").GetID();
        auto& ragdoll = Actor().AddComponent<scene::RagdollComponent>();
        ragdoll.rootBoneName = "Body";
        ragdoll.gravity = 10.0f;
        ragdoll.linearDrag = 0.0f;
        ragdoll.groundPlane = false;
        ragdoll.contactWorld = false;
        ragdoll.contactDynamic = false;
        ragdoll.contactSelf = false;
        ragdoll.blendIn = 0.0f;
    }

    scene::GameObject& Actor() { return *m_scene.GetGameObject(m_actor); }
    scene::RagdollComponent& Ragdoll() { return *Actor().GetComponent<scene::RagdollComponent>(); }

    void Tick(float dt = 1.0f / 60.0f, bool simulating = true)
    {
        SystemContext context{m_scene, m_world, nullptr, nullptr, dt, 1.0f / 60.0f,
                              simulating, true};
        scene::RagdollSystem system;
        system.Update(context);
    }

    void PrepareModel()
    {
        m_model.skeleton = std::make_unique<asset::Skeleton>();
        auto& skeleton = *m_model.skeleton;
        skeleton.rootNodeIndex = 0;
        skeleton.nodes.resize(2);
        skeleton.bones.resize(2);
        for (int i = 0; i < 2; ++i) {
            auto& node = skeleton.nodes[static_cast<std::size_t>(i)];
            node.name = i == 0 ? "Body" : "Tip";
            node.parentIndex = i - 1;
            node.boneIndex = i;
            node.localBindTransform = math::Matrix4::TRS({0, static_cast<float>(i), 0},
                math::Quaternion::Identity(), math::Vector3::ONE);
            skeleton.nodeMap[node.name] = i;
            skeleton.bones[static_cast<std::size_t>(i)].nodeIndex = i;
        }
        skeleton.nodes[0].children = {1};
        Actor().AddComponent<scene::SkinnedMeshRenderer>().model = &m_model;
        Actor().GetComponent<scene::AnimatorComponent>()->nodeGlobalTransforms = {
            math::Matrix4::Identity(), skeleton.nodes[1].localBindTransform};
        Actor().GetComponent<scene::AnimatorComponent>()->boneMatrices.resize(2);
    }

    void PrepareBones()
    {
        std::vector<scene::EntityID> ids;
        auto parent = m_actor;
        for (int i = 0; i < 2; ++i) {
            auto& bone = m_scene.CreateGameObject(i == 0 ? "Body" : "Tip");
            bone.SetParent(*m_scene.GetGameObject(parent));
            bone.transform.position = {0, static_cast<float>(i), 0};
            bone.transform.worldPosition = bone.transform.position;
            ids.push_back(bone.GetID());
            parent = bone.GetID();
        }
        Actor().GetComponent<scene::SkinnedMeshRenderer>()->nodeEntities = ids;
    }
};

TEST_F(RagdollSystemTest, StartWaitsForAnimatorModelAndBoneEntities)
{
    Ragdoll().activateOnStart = true;
    Tick();
    EXPECT_EQ(Ragdoll().runtimeStatus, scene::RagdollStatus::NoAnimator);
    EXPECT_TRUE(Ragdoll().beginRequested);
    EXPECT_FALSE(Ragdoll().startTriggered);
    Actor().AddComponent<scene::AnimatorComponent>();
    Tick();
    EXPECT_EQ(Ragdoll().runtimeStatus, scene::RagdollStatus::NoSkinnedMesh);
    EXPECT_TRUE(Ragdoll().beginRequested);
    PrepareModel();
    Tick();
    EXPECT_EQ(Ragdoll().runtimeStatus, scene::RagdollStatus::NoBones);
    EXPECT_TRUE(Ragdoll().beginRequested);
    PrepareBones();
    Tick();
    EXPECT_EQ(Ragdoll().runtimeStatus, scene::RagdollStatus::Running);
    EXPECT_TRUE(Ragdoll().IsStanding());
    EXPECT_TRUE(Ragdoll().startTriggered);
    EXPECT_FALSE(Ragdoll().beginRequested);
}

TEST_F(RagdollSystemTest, DisableInactiveHierarchyAndEndReleaseAllGrips)
{
    Actor().AddComponent<scene::AnimatorComponent>();PrepareModel();PrepareBones();
    Ragdoll().activateOnStart=true;Ragdoll().blendOut=0;
    for(int action=0;action<3;++action) {
        Ragdoll().enabled=true;Actor().SetActive(true);
        Ragdoll().beginRequested=true;Ragdoll().activeRequested=true;Tick();
        ASSERT_NE(Ragdoll().runtime.rig,nullptr);
        auto& controls=Ragdoll().runtime.rig->Controls();
        const auto body=controls.BodyState("Body");ASSERT_TRUE(body.valid);
        physics::XPBDGripSettings settings;settings.breakForce=0;
        ASSERT_TRUE(controls.Grab("Body",nullptr,body.position,settings));
        if(action==0)Ragdoll().enabled=false;
        if(action==1)Actor().SetActive(false);
        if(action==2)Ragdoll().endRequested=true;
        Tick();
        EXPECT_EQ(controls.GrabState("Body").status,physics::XPBDGripStatus::RELEASED);
        EXPECT_FALSE(controls.HasGrabs());
    }
}

TEST_F(RagdollSystemTest, PauseReleasesGripsWhenDisabledOrAncestorInactive)
{
    auto& parent = m_scene.CreateGameObject("Parent");
    Actor().SetParent(parent);
    Actor().AddComponent<scene::AnimatorComponent>();
    PrepareModel();
    PrepareBones();
    Ragdoll().gravity = 0.0f;
    Ragdoll().blendOut = 0.0f;

    for (const bool disableComponent : {true, false}) {
        SCOPED_TRACE(disableComponent ? "disabled component" : "inactive ancestor");
        parent.SetActive(true);
        Ragdoll().enabled = true;
        Ragdoll().beginRequested = true;
        Ragdoll().activeRequested = true;
        Tick();
        ASSERT_NE(Ragdoll().runtime.rig, nullptr);
        auto& controls = Ragdoll().runtime.rig->Controls();
        const auto body = controls.BodyState("Body");
        ASSERT_TRUE(body.valid);
        physics::XPBDGripSettings settings;
        settings.breakForce = 0.0f;
        ASSERT_TRUE(controls.Grab("Body", nullptr, body.position, settings));
        ASSERT_EQ(controls.GrabState("Body").status, physics::XPBDGripStatus::HOLDING);
        Ragdoll().endRequested = true;

        Tick(0.5f, false);

        EXPECT_EQ(controls.GrabState("Body").status, physics::XPBDGripStatus::HOLDING);
        EXPECT_TRUE(controls.HasGrabs());
        EXPECT_TRUE(Ragdoll().endRequested);

        if (disableComponent) Ragdoll().enabled = false;
        else parent.SetActive(false);

        Tick(0.5f, false);

        EXPECT_EQ(controls.GrabState("Body").status, physics::XPBDGripStatus::RELEASED);
        EXPECT_FALSE(controls.HasGrabs());
    }
}

TEST_F(RagdollSystemTest, PhysicsAssetReloadIsTransactionalAndSerializedReferenceSurvives)
{
    Actor().AddComponent<scene::AnimatorComponent>(); PrepareModel(); PrepareBones();
    testkit::TempDir temp("ragdoll-system"); ASSERT_TRUE(temp.IsValid());
    const auto path=temp.File("Actor.ragdoll").string();
    scene::RagdollDefinition definition;
    scene::RagdollBodyDefinition body; body.bone="Body"; body.shape=scene::RagdollBodyShape::SPHERE; body.mass=7;
    definition.bodies.push_back(body);
    ASSERT_TRUE(scene::SaveRagdollDefinition(path,definition));
    Ragdoll().physicsAssetPath=path; Ragdoll().activateOnStart=true;
    Tick();
    ASSERT_EQ(Ragdoll().runtimeStatus,scene::RagdollStatus::Running);
    EXPECT_NEAR(Ragdoll().runtime.rig->GetBody(0)->GetMass(),7,1.0e-6f);
    auto* original=Ragdoll().runtime.rig.get();
    ASSERT_TRUE(util::FileSystem::WriteTextAtomic(path,"[ragdoll]\nversion=99\n"));
    asset::AssetManager::BumpAssetGeneration(); Tick();
    EXPECT_EQ(Ragdoll().runtime.rig.get(),original);
    EXPECT_NEAR(Ragdoll().runtime.rig->GetBody(0)->GetMass(),7,1.0e-6f);
    definition.bodies[0].mass=11;
    ASSERT_TRUE(scene::SaveRagdollDefinition(path,definition));
    asset::AssetManager::BumpAssetGeneration(); Tick();
    EXPECT_NEAR(Ragdoll().runtime.rig->GetBody(0)->GetMass(),11,1.0e-6f);
    EXPECT_EQ(Ragdoll().runtimeStatus,scene::RagdollStatus::Running);
    scene::RagdollComponent copied=Ragdoll();
    EXPECT_EQ(copied.physicsAssetPath,path);
    EXPECT_FALSE(copied.runtime.definitionValid);
    EXPECT_EQ(copied.runtime.rig,nullptr);
}

TEST_F(RagdollSystemTest, StandingGuardTransportsOwnerMotionWithoutTransportingAnimatedRootBob)
{
    Actor().AddComponent<scene::AnimatorComponent>();
    PrepareModel();
    PrepareBones();
    testkit::TempDir temp("ragdoll-owner-frame");
    ASSERT_TRUE(temp.IsValid());
    scene::RagdollDefinition definition;
    for (const auto* name : {"Body", "Tip"}) {
        scene::RagdollBodyDefinition body;
        body.bone = name;
        body.shape = scene::RagdollBodyShape::SPHERE;
        body.radius = 0.1f;
        body.mass = 1.0f;
        definition.bodies.push_back(body);
    }
    const auto path = temp.File("OwnerFrame.ragdoll").string();
    ASSERT_TRUE(scene::SaveRagdollDefinition(path, definition));
    Ragdoll().physicsAssetPath = path;
    Ragdoll().gravity = 0.0f;
    Ragdoll().standingGuard = true;
    Ragdoll().beginRequested = true;
    Ragdoll().activeRequested = true;
    Tick(0.0f);
    ASSERT_EQ(Ragdoll().runtimeStatus, scene::RagdollStatus::Running);
    ASSERT_EQ(Ragdoll().runtimeBodyCount, 2);
    ASSERT_NE(Ragdoll().runtime.rig, nullptr);
    auto& controls = Ragdoll().runtime.rig->Controls();
    const auto initialTip = controls.BodyState("Tip");
    ASSERT_TRUE(initialTip.valid);
    const auto* renderer = Actor().GetComponent<scene::SkinnedMeshRenderer>();
    ASSERT_NE(renderer, nullptr);
    ASSERT_EQ(renderer->nodeEntities.size(), 2u);
    auto* bodyBone = m_scene.GetGameObject(renderer->nodeEntities[0]);
    auto* tipBone = m_scene.GetGameObject(renderer->nodeEntities[1]);
    ASSERT_NE(bodyBone, nullptr);
    ASSERT_NE(tipBone, nullptr);
    const auto tipFkPosition = tipBone->transform.worldPosition;

    bodyBone->transform.position.y += 0.2f;
    tipBone->transform.position.y -= 0.2f;
    scene::FlushWorldTransforms(m_scene);
    ASSERT_VEC3_NEAR(tipBone->transform.worldPosition, tipFkPosition, testkit::kTolerance);
    Tick(0.0f);

    const auto afterBob = controls.BodyState("Tip");
    ASSERT_TRUE(afterBob.valid);
    EXPECT_VEC3_NEAR(afterBob.position, initialTip.position, testkit::kTolerance);
    EXPECT_VEC3_NEAR(afterBob.velocity, initialTip.velocity, testkit::kTolerance);

    const auto beforeOwnerBody = controls.BodyState("Body");
    const auto beforeOwnerTip = controls.BodyState("Tip");
    ASSERT_TRUE(beforeOwnerBody.valid);
    ASSERT_TRUE(beforeOwnerTip.valid);
    const math::Vector3 ownerDelta{3.0f, 0.0f, 2.0f};
    Actor().transform.position += ownerDelta;
    scene::FlushWorldTransforms(m_scene);
    Tick(0.0f);

    EXPECT_VEC3_NEAR(controls.BodyState("Body").position,
                     beforeOwnerBody.position + ownerDelta, testkit::kTolerance);
    EXPECT_VEC3_NEAR(controls.BodyState("Tip").position,
                     beforeOwnerTip.position + ownerDelta, testkit::kTolerance);
    Tick(0.0f);
    EXPECT_VEC3_NEAR(controls.BodyState("Body").position,
                     beforeOwnerBody.position + ownerDelta, testkit::kTolerance);
    EXPECT_VEC3_NEAR(controls.BodyState("Tip").position,
                     beforeOwnerTip.position + ownerDelta, testkit::kTolerance);

    ASSERT_TRUE(controls.Impulse("Body", {1.0f, 0.5f, 0.0f}));
    ASSERT_VEC3_NEAR(controls.BodyState("Body").velocity,
                     (math::Vector3{1.0f, 0.5f, 0.0f}), testkit::kTolerance);
    Actor().transform.rotation = math::Quaternion::FromAxisAngle(math::Vector3::FORWARD, math::PI * 0.5f);
    scene::FlushWorldTransforms(m_scene);

    for (int frame = 0; frame < 2; ++frame) {
        SCOPED_TRACE(frame);
        Tick(0.0f);

        const auto rotatedBody = controls.BodyState("Body");
        const auto rotatedTip = controls.BodyState("Tip");
        ASSERT_TRUE(rotatedBody.valid);
        ASSERT_TRUE(rotatedTip.valid);
        EXPECT_VEC3_NEAR(rotatedBody.position, ownerDelta, testkit::kTolerance);
        EXPECT_VEC3_NEAR(rotatedTip.position, (math::Vector3{2.0f, 0.0f, 2.0f}), testkit::kTolerance);
        EXPECT_VEC3_NEAR(rotatedBody.velocity, (math::Vector3{-0.5f, 1.0f, 0.0f}), testkit::kTolerance);
        EXPECT_VEC3_NEAR(rotatedTip.velocity, math::Vector3::ZERO, testkit::kTolerance);
        for (const auto& state : {rotatedBody, rotatedTip}) {
            EXPECT_VEC3_NEAR(state.rotation * math::Vector3::RIGHT, math::Vector3::UP, testkit::kTolerance);
            EXPECT_VEC3_NEAR(state.rotation * math::Vector3::UP, -math::Vector3::RIGHT, testkit::kTolerance);
        }
    }
}

TEST_F(RagdollSystemTest, SamplesMovingDriveTargetsAtPhysicalTimesAcrossFrameRates)
{
    Actor().AddComponent<scene::AnimatorComponent>();
    PrepareModel();
    PrepareBones();
    testkit::TempDir temp("ragdoll-target-time");
    ASSERT_TRUE(temp.IsValid());
    scene::RagdollDefinition definition;
    scene::RagdollBodyDefinition body;
    body.bone = "Body";
    body.shape = scene::RagdollBodyShape::SPHERE;
    body.radius = 0.1f;
    body.mass = 1.0f;
    body.drive.enabled = true;
    body.drive.space = scene::RagdollDriveSpace::WORLD;
    body.drive.linearFrequency = 8.0f;
    body.drive.angularFrequency = 0.0f;
    body.drive.maxForce = 1000.0f;
    body.drive.maxTorque = 0.0f;
    definition.bodies.push_back(body);
    const auto path = temp.File("MovingTarget.ragdoll").string();
    ASSERT_TRUE(scene::SaveRagdollDefinition(path, definition));
    Ragdoll().physicsAssetPath = path;
    Ragdoll().gravity = 0.0f;
    Ragdoll().substeps = 12;
    const auto* renderer = Actor().GetComponent<scene::SkinnedMeshRenderer>();
    ASSERT_NE(renderer, nullptr);
    auto* bodyBone = m_scene.GetGameObject(renderer->nodeEntities[0]);
    ASSERT_NE(bodyBone, nullptr);

    for (const int fps : {30, 60, 144, 240}) {
        SCOPED_TRACE(fps);
        bodyBone->transform.position = math::Vector3::ZERO;
        scene::FlushWorldTransforms(m_scene);
        Ragdoll().enabled = true;
        Ragdoll().beginRequested = true;
        Ragdoll().activeRequested = true;
        Tick(0.0f);
        ASSERT_EQ(Ragdoll().runtimeStatus, scene::RagdollStatus::Running);
        auto& controls = Ragdoll().runtime.rig->Controls();
        ASSERT_TRUE(controls.Impulse("Body", math::Vector3::RIGHT));
        const float dt = 1.0f / static_cast<float>(fps);
        float maximumSpeedError = 0.0f;
        double remainingTime = 0.0;
        int physicalSteps = 0;

        for (int frame = 1; frame <= fps; ++frame) {
            physicalSteps += scene::AccumulateRagdollSteps(remainingTime, dt);
            bodyBone->transform.position.x = static_cast<float>(frame) * dt;
            scene::FlushWorldTransforms(m_scene);
            Tick(dt);
            const auto state = controls.BodyState("Body");
            ASSERT_TRUE(state.valid);
            maximumSpeedError = math::Max(maximumSpeedError, std::abs(state.velocity.x - 1.0f));
        }

        EXPECT_LT(maximumSpeedError, 0.002f);
        EXPECT_NEAR(controls.BodyState("Body").position.x,
            static_cast<float>(physicalSteps) * scene::RAGDOLL_FIXED_STEP, 0.002f);
        Ragdoll().enabled = false;
        Tick(0.0f);
    }

    bodyBone->transform.position = math::Vector3::ZERO;
    scene::FlushWorldTransforms(m_scene);
    Ragdoll().enabled = true;
    Ragdoll().beginRequested = true;
    Ragdoll().activeRequested = true;
    Tick(0.0f);
    auto& controls = Ragdoll().runtime.rig->Controls();
    ASSERT_TRUE(controls.Impulse("Body", math::Vector3::RIGHT));
    double sampledTime = 0.0;
    double remainingTime = 0.0;
    int physicalSteps = 0;

    for (int frame = 0; frame < 32; ++frame) {
        SCOPED_TRACE(frame);
        const float dt = frame == 0 ? 0.5f : frame == 1 ? 0.2f : 1.0f / 144.0f;
        sampledTime += static_cast<double>(dt);
        physicalSteps += scene::AccumulateRagdollSteps(remainingTime, dt);
        bodyBone->transform.position.x = static_cast<float>(sampledTime);
        scene::FlushWorldTransforms(m_scene);
        Tick(dt);

        EXPECT_NEAR(controls.BodyState("Body").velocity.x, 1.0f, 0.002f);
        if (frame < 2)
            EXPECT_NEAR(controls.BodyState("Body").position.x,
                static_cast<float>((frame + 1) * 16) * scene::RAGDOLL_FIXED_STEP, 0.002f);
    }
    EXPECT_NEAR(controls.BodyState("Body").position.x,
        static_cast<float>(physicalSteps) * scene::RAGDOLL_FIXED_STEP, 0.002f);

    auto& rig = *Ragdoll().runtime.rig;
    rig.SetStandingGuard(true, 0.1f, 0.1f);
    const auto beforeRejectedSample = controls.BodyState("Body");
    auto rejectedTargets = Ragdoll().runtime.bones;
    rejectedTargets[0].position.x += 100.0f;
    for (const float invalidDt : {-1.0f, std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity()}) {
        rig.UpdateDriveTargets(rejectedTargets, invalidDt);
        EXPECT_VEC3_NEAR(controls.BodyState("Body").position,
            beforeRejectedSample.position, testkit::kTolerance);
        EXPECT_VEC3_NEAR(controls.BodyState("Body").velocity,
            beforeRejectedSample.velocity, testkit::kTolerance);
    }
}

TEST_F(RagdollSystemTest, FifteenFpsAdvancesAFullSecondOfGravityAndFinalBounds)
{
    Actor().AddComponent<scene::AnimatorComponent>();
    PrepareModel();
    PrepareBones();
    Ragdoll().beginRequested = true;
    for (int frame = 0; frame < 15; ++frame) Tick(1.0f / 15.0f);
    ASSERT_NE(Ragdoll().runtime.rig, nullptr);
    ASSERT_NE(Ragdoll().runtime.rig->BodyOfBone(0), nullptr);
    const auto* body = Ragdoll().runtime.rig->BodyOfBone(0);
    EXPECT_NEAR(body->GetPosition().y, -4.5f, 0.2f);
    const auto* animator = Actor().GetComponent<scene::AnimatorComponent>();
    EXPECT_NEAR(animator->skinnedBoundsCenter.y, body->GetPosition().y, 0.001f);
    EXPECT_NEAR(animator->skinnedBoundsRadius, 0.5f, 0.001f);
}

TEST_F(RagdollSystemTest, EndingDuringBlendInDoesNotJumpToFullWeight)
{
    auto& ragdoll = Ragdoll();
    ragdoll.phase = scene::RagdollPhase::BlendIn;
    ragdoll.activationWeight = 1.0f;
    ragdoll.weight = 0.2f;
    ragdoll.blendOut = 0.4f;
    scene::BeginRagdollBlendOut(ragdoll);
    EXPECT_NEAR(ragdoll.weight, 0.2f, 1.0e-6f);
    scene::AdvanceRagdollPhase(ragdoll, 1.0f / 60.0f);
    EXPECT_NEAR(ragdoll.weight, 0.2f * (1.0f - (1.0f / 60.0f) / 0.4f), 1.0e-6f);
}

TEST_F(RagdollSystemTest, FixedClockPreservesElapsedTimeAcrossFrameRates)
{
    for (int fps : {15, 30, 60, 120, 240}) {
        double remaining = 0.0;
        int steps = 0;
        for (int frame = 0; frame < fps; ++frame)
            steps += scene::AccumulateRagdollSteps(remaining, 1.0f / static_cast<float>(fps));
        EXPECT_EQ(steps, 120) << fps;
        EXPECT_NEAR(remaining, 0.0, 1.0e-6);
    }
}

TEST_F(RagdollSystemTest, CatchUpRetainsBacklogAndRejectsInvalidDelta)
{
    double remaining = 0.0;
    int steps = scene::AccumulateRagdollSteps(remaining, 0.5f);
    EXPECT_EQ(steps, 16);
    EXPECT_GT(remaining, 0.3);
    for (int i = 0; i < 4; ++i) steps += scene::AccumulateRagdollSteps(remaining, 0.0f);
    EXPECT_NEAR(steps * scene::RAGDOLL_FIXED_STEP + remaining, 0.5, 1.0e-6);
    const auto previous = remaining;
    EXPECT_EQ(scene::AccumulateRagdollSteps(remaining, std::numeric_limits<float>::quiet_NaN()), 0);
    EXPECT_NEAR(remaining, previous, 1.0e-9);
}

TEST_F(RagdollSystemTest, ExcludedBranchesRebuildAndOverrideAdditionalRoots)
{
    Actor().AddComponent<scene::AnimatorComponent>();
    PrepareModel();
    PrepareBones();
    auto& skeleton = *m_model.skeleton;
    skeleton.nodes.resize(4);
    skeleton.bones.resize(4);
    skeleton.nodes[0].children.push_back(2);
    skeleton.nodes[2].children = {3};
    auto parent = Actor().GetComponent<scene::SkinnedMeshRenderer>()->nodeEntities[0];
    for (int i = 2; i < 4; ++i) {
        auto& node = skeleton.nodes[static_cast<std::size_t>(i)];
        node.name = i == 2 ? "Branch_A" : "Tip_A";
        node.parentIndex = i == 2 ? 0 : 2;
        node.boneIndex = i;
        const math::Vector3 local = i == 2 ? math::Vector3{1, 0, 0} : math::Vector3{0, -1, 0};
        const math::Vector3 world = i == 2 ? math::Vector3{1, 0, 0} : math::Vector3{1, -1, 0};
        node.localBindTransform = math::Matrix4::TRS(local, math::Quaternion::Identity(), math::Vector3::ONE);
        skeleton.bones[static_cast<std::size_t>(i)].nodeIndex = i;
        auto& bone = m_scene.CreateGameObject(node.name);
        bone.SetParent(*m_scene.GetGameObject(parent));
        bone.transform.position = local;
        bone.transform.worldPosition = world;
        parent = bone.GetID();
        Actor().GetComponent<scene::SkinnedMeshRenderer>()->nodeEntities.push_back(parent);
        Actor().GetComponent<scene::AnimatorComponent>()->nodeGlobalTransforms.push_back(
            math::Matrix4::TRS(world, math::Quaternion::Identity(), math::Vector3::ONE));
    }
    Actor().GetComponent<scene::AnimatorComponent>()->boneMatrices.resize(4);
    Ragdoll().gravity = 0.0f;
    Ragdoll().beginRequested = true;
    Ragdoll().activeRequested = true;
    Ragdoll().standingGuard = true;
    Tick();
    EXPECT_EQ(Ragdoll().runtime.bones.size(), 4u);
    EXPECT_EQ(Ragdoll().runtimeBodyCount, 2);
    Ragdoll().excludedRootBones = { "Branch_A" };
    Ragdoll().extraRootBones = { "Tip_A" };
    Tick();
    EXPECT_EQ(Ragdoll().runtime.bones.size(), 2u);
    EXPECT_EQ(Ragdoll().runtimeBodyCount, 1);
    EXPECT_TRUE(Ragdoll().IsStanding());
    Ragdoll().excludedRootBones.clear();
    Tick();
    EXPECT_EQ(Ragdoll().runtime.bones.size(), 4u);
}

TEST_F(RagdollSystemTest, BoneSelectionListsSurviveSerialization)
{
    auto& source = Ragdoll();
    source.extraRootBones = { "Branch_A", "Branch_B" };
    source.excludedRootBones = { "Tip_A", "Accessory" };
    const auto data = scene::SerializeReflected(source);
    scene::RagdollComponent restored;
    scene::DeserializeReflected(data, restored);
    EXPECT_EQ(restored.extraRootBones, source.extraRootBones);
    EXPECT_EQ(restored.excludedRootBones, source.excludedRootBones);
}

TEST_F(RagdollSystemTest, PauseDoesNotAdvancePhysicsOrPlayback)
{
    Actor().AddComponent<scene::AnimatorComponent>();
    PrepareModel();
    PrepareBones();
    Ragdoll().beginRequested = true;
    Tick();
    const auto before = Ragdoll().runtime.rig->BodyOfBone(0)->GetPosition();
    const float phaseTimer = Ragdoll().phaseTimer;
    Tick(1.0f, false);
    EXPECT_VEC3_NEAR(Ragdoll().runtime.rig->BodyOfBone(0)->GetPosition(), before, 1.0e-6f);
    EXPECT_NEAR(Ragdoll().phaseTimer, phaseTimer, 1.0e-6f);
}

TEST_F(RagdollSystemTest, PausePreservesGuardPoseAndDefersPendingInputUntilResume)
{
    Actor().AddComponent<scene::AnimatorComponent>();
    PrepareModel();
    PrepareBones();
    Ragdoll().gravity = 0.0f;
    Ragdoll().standingGuard = true;
    Ragdoll().beginRequested = true;
    Ragdoll().activeRequested = true;
    Tick();
    ASSERT_NE(Ragdoll().runtime.rig, nullptr);
    const auto* body = Ragdoll().runtime.rig->BodyOfBone(0);
    ASSERT_NE(body, nullptr);
    const auto position = body->GetPosition();
    const auto rotation = body->GetRotation();
    const auto velocity = body->GetVelocity();
    const auto angularVelocity = body->GetAngularVelocity();
    const auto phase = Ragdoll().phase;
    const float phaseTimer = Ragdoll().phaseTimer;
    Actor().transform.position = {3.0f, 0.0f, 0.0f};
    Actor().transform.rotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, 0.4f);
    scene::FlushWorldTransforms(m_scene);
    scene::RagdollImpulse impulse;
    impulse.velocity = {1.0f, 0.0f, 0.0f};
    Ragdoll().pendingImpulses.push_back(impulse);
    Ragdoll().endRequested = true;

    Tick(0.5f, false);

    EXPECT_VEC3_NEAR(body->GetPosition(), position, testkit::kTolerance);
    EXPECT_VEC3_NEAR(body->GetRotation() * math::Vector3::RIGHT,
                     rotation * math::Vector3::RIGHT, testkit::kTolerance);
    EXPECT_VEC3_NEAR(body->GetRotation() * math::Vector3::UP,
                     rotation * math::Vector3::UP, testkit::kTolerance);
    EXPECT_VEC3_NEAR(body->GetVelocity(), velocity, testkit::kTolerance);
    EXPECT_VEC3_NEAR(body->GetAngularVelocity(), angularVelocity, testkit::kTolerance);
    EXPECT_EQ(Ragdoll().phase, phase);
    EXPECT_NEAR(Ragdoll().phaseTimer, phaseTimer, testkit::kTolerance);
    EXPECT_EQ(Ragdoll().pendingImpulses.size(), 1u);
    EXPECT_TRUE(Ragdoll().endRequested);

    Tick();

    EXPECT_GT(body->GetPosition().x, 2.5f);
    EXPECT_TRUE(Ragdoll().pendingImpulses.empty());
    EXPECT_FALSE(Ragdoll().endRequested);
    EXPECT_EQ(Ragdoll().phase, scene::RagdollPhase::BlendOut);
}

TEST_F(RagdollSystemTest, ManagedRemovalReleasesActiveContactSolverAndReaddGetsFreshRig)
{
    Actor().AddComponent<scene::AnimatorComponent>(); PrepareModel(); PrepareBones();
    Ragdoll().beginRequested = true; Ragdoll().contactWorld = true;
    physics::OBBCollider floor{{20, 1, 20}}; floor.Update({0, -1, 0}, math::Quaternion::Identity());
    const physics::PhysicsMaterial material;
    m_world.BeginSceneSync(); m_world.SyncCollider({}, physics::ColliderInstance{&floor, nullptr, &material, math::Vector3::ZERO, false, 0}); m_world.EndSceneSync();
    Tick();
    ASSERT_NE(Ragdoll().runtime.rig, nullptr);
    auto& rig = *Ragdoll().runtime.rig; ASSERT_GT(rig.GetBodyCount(), 0);
    ASSERT_NE(rig.GetBody(0), nullptr); rig.GetBody(0)->SetPosition({0, -0.1f, 0}); rig.RefreshContacts(&m_world);
    ASSERT_GT(rig.GetContactCount(), 0);
    const uint64_t original = m_scene.GetComponentLifetime<scene::RagdollComponent>(m_actor);
    const uint64_t animator = m_scene.GetComponentLifetime<scene::AnimatorComponent>(m_actor);
    using Controls = scene::managed_detail::ManagedObjectControls;
    const auto kind = static_cast<uint32_t>(scene::ManagedNativeComponentKind::RAGDOLL);
    bool exists = false; uint64_t held = original;
    ASSERT_EQ(Controls::Component(Actor(), kind, scene::ManagedBridgeOp::COMPONENT_REMOVE, exists), scene::ManagedBridgeStatus::OK);
    EXPECT_TRUE(exists);
    ASSERT_EQ(Controls::Component(Actor(), kind, scene::ManagedBridgeOp::COMPONENT_READ, exists, &held), scene::ManagedBridgeStatus::OK);
    EXPECT_FALSE(exists); EXPECT_EQ(held, 0U);
    Controls::FlushScriptRemovals(m_scene);
    EXPECT_EQ(Actor().GetComponent<scene::RagdollComponent>(), nullptr);
    EXPECT_EQ(m_scene.GetComponentLifetime<scene::AnimatorComponent>(m_actor), animator);
    EXPECT_TRUE(m_world.GetConstraints().empty()); m_world.Step(1.0f / 60.0f); Tick();
    const auto hits = m_world.RaycastAll({0, 2, 0}, {0, -1, 0}, 10);
    ASSERT_EQ(hits.size(), 1U); EXPECT_EQ(hits[0].collider, &floor);
    uint64_t replacement = 0;
    ASSERT_EQ(Controls::Component(Actor(), kind, scene::ManagedBridgeOp::COMPONENT_ADD, exists, &replacement), scene::ManagedBridgeStatus::OK);
    EXPECT_TRUE(exists); EXPECT_NE(replacement, original); EXPECT_EQ(Ragdoll().runtime.rig, nullptr);
    EXPECT_FALSE(Ragdoll().beginRequested); EXPECT_TRUE(Ragdoll().pendingImpulses.empty());
    Ragdoll().beginRequested = true; Tick(); ASSERT_NE(Ragdoll().runtime.rig, nullptr); EXPECT_GT(Ragdoll().runtimeBodyCount, 0);
    m_world.BeginSceneSync(); m_world.EndSceneSync();
}

} /// @note namespace fbzz::tests
