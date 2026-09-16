/// @file    RagdollSystemTests.cpp
/// @brief   描画デバイスに依存せず起動待ち・時計・復帰・最終姿勢の境界を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-13
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/ComponentReflectionCodec.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Ragdoll/RagdollPlayback.hpp>
#include <Engine/Scene/SkinnedPoseBounds.hpp>
#include <Engine/Scene/Systems/RagdollSystem.hpp>
#include <Physics/World.hpp>
#include <limits>

namespace fbzz::tests {

namespace {
class RagdollProbe final : public scene::Script {};
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

TEST_F(RagdollSystemTest, EndCancelsBeginWhileAssetsAreStillMissing)
{
    RagdollProbe probe;
    probe.SetContext(&m_scene, &Actor());
    probe.ragdoll.BeginActive();
    Tick();
    EXPECT_TRUE(Ragdoll().beginRequested);
    probe.ragdoll.End();
    Actor().AddComponent<scene::AnimatorComponent>();
    PrepareModel();
    PrepareBones();
    Tick();
    EXPECT_FALSE(Ragdoll().beginRequested);
    EXPECT_EQ(Ragdoll().phase, scene::RagdollPhase::Idle);
}

TEST_F(RagdollSystemTest, BeginActiveDuringBlendOutContinuesFromCurrentWeight)
{
    auto& ragdoll = Ragdoll();
    ragdoll.mode = scene::RagdollMode::Active;
    ragdoll.phase = scene::RagdollPhase::BlendOut;
    ragdoll.weight = 0.2f;
    ragdoll.blendIn = 0.4f;
    RagdollProbe probe;
    probe.SetContext(&m_scene, &Actor());
    probe.ragdoll.BeginActive();
    EXPECT_NEAR(ragdoll.weight, 0.2f, 1.0e-6f);
    scene::AdvanceRagdollPhase(ragdoll, 1.0f / 60.0f);
    EXPECT_NEAR(ragdoll.weight, 0.2f + 0.8f * (1.0f / 60.0f) / 0.4f, 1.0e-6f);
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

TEST_F(RagdollSystemTest, StandingGuardOnlyChangesItsOwnSettings)
{
    auto& component = Ragdoll();
    component.profile = scene::RagdollProfileKind::Mech;
    component.gravity = 3.0f;
    component.rootAnchor = 2.5f;
    component.rootAnchorTilt = 0.4f;
    component.contactSelf = true;
    component.contactWhileActive = true;
    component.groundPlane = true;
    component.collapseDistance = 1.2f;
    RagdollProbe probe;
    probe.SetContext(&m_scene, &Actor());
    probe.ragdoll.SetStandingGuard(true, 2.0f, 60.0f);
    EXPECT_TRUE(component.standingGuard);
    EXPECT_NEAR(component.standingMaxDistance, 2.0f, 1.0e-6f);
    EXPECT_NEAR(component.standingMaxDegrees, 60.0f, 1.0e-6f);
    EXPECT_EQ(component.profile, scene::RagdollProfileKind::Mech);
    EXPECT_NEAR(component.gravity, 3.0f, 1.0e-6f);
    EXPECT_NEAR(component.rootAnchor, 2.5f, 1.0e-6f);
    EXPECT_NEAR(component.rootAnchorTilt, 0.4f, 1.0e-6f);
    EXPECT_TRUE(component.contactSelf);
    EXPECT_TRUE(component.contactWhileActive);
    EXPECT_TRUE(component.groundPlane);
    EXPECT_NEAR(component.collapseDistance, 1.2f, 1.0e-6f);
    EXPECT_FALSE(component.beginRequested);
    probe.ragdoll.SetStandingGuard(false, 2.0f, 60.0f);
    EXPECT_FALSE(component.standingGuard);
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

TEST_F(RagdollSystemTest, GuardDoesNotConvertPassiveRequestsToActive)
{
    Actor().AddComponent<scene::AnimatorComponent>();
    PrepareModel();
    PrepareBones();
    RagdollProbe probe;
    probe.SetContext(&m_scene, &Actor());
    probe.ragdoll.SetStandingGuard(true, 0.2f, 20.0f);
    probe.ragdoll.Begin();
    Tick();
    EXPECT_EQ(Ragdoll().mode, scene::RagdollMode::Passive);
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

} // namespace fbzz::tests
