/// @file    FootIKTests.cpp
/// @brief   足接地と部位 IK の骨長・姿勢・腰上下限・重み・保存契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-03
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/Engine/ResourceTrackingRenderer.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/IKTargetReferences.hpp>
#include <Engine/Scene/Systems/IKSystem.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/RagdollComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Physics/World.hpp>
#include <Physics/OBBCollider.hpp>
#include <array>
#include <cmath>
#include <limits>

namespace fbzz::tests {
class FootIKTest : public testkit::EngineFixture {
protected:
    testkit::ResourceTrackingRenderer m_renderer;
    renderer::ResourceManager m_resources{m_renderer};
    asset::Model m_model;
    scene::Scene m_scene;
    physics::World m_world;
    physics::OBBCollider m_floor{{5,0.5f,5}};
    physics::PhysicsMaterial m_material;
    scene::GameObject* m_actor = nullptr;
    std::array<scene::GameObject*,9> m_nodes{};
    std::array<math::Vector3,9> m_fk{{{0,1,0},{-0.1f,1,0},{-0.1f,0.55f,0.05f},{-0.1f,0.1f,0},
        {-0.1f,0.05f,0.1f},{0.1f,1,0},{0.1f,0.55f,0.05f},{0.1f,0.1f,0},{0.1f,0.05f,0.1f}}};
    void SetUp() override {
        EngineFixture::SetUp();
        m_model.skeleton = std::make_unique<asset::Skeleton>();
        auto& skeleton = *m_model.skeleton;
        const char* names[]{"Hips","LeftUpLeg","LeftLeg","LeftFoot","LeftToeBase","RightUpLeg","RightLeg","RightFoot","RightToeBase"};
        const int parents[]{-1,0,1,2,3,0,5,6,7};
        skeleton.rootNodeIndex = 0; skeleton.nodes.resize(9);
        m_actor = &m_scene.CreateGameObject("Actor");
        auto& smr = m_actor->AddComponent<scene::SkinnedMeshRenderer>(); smr.model = &m_model;
        auto& animator = m_actor->AddComponent<scene::AnimatorComponent>(); animator.nodeGlobalTransforms.resize(9);
        for (int i=0;i<9;++i) {
            auto& node = skeleton.nodes[i]; node.name=names[i]; node.parentIndex=parents[i];
            skeleton.nodeMap[node.name]=i;
            if (parents[i]>=0) skeleton.nodes[parents[i]].children.push_back(i);
            m_nodes[i]=&m_scene.CreateGameObject(names[i]);
            m_nodes[i]->SetParent(parents[i]<0 ? *m_actor : *m_nodes[parents[i]]);
            smr.nodeEntities.push_back(m_nodes[i]->GetID());
        }
        auto& chain = m_actor->AddComponent<scene::IKSolverComponent>().chains.emplace_back();
        chain.type=scene::IKSolverType::FootPlace; chain.targetName="Feet";
        chain.useAnimatorIKWeight=false; chain.footSurfaceOffset=0.1f; chain.correctionDeadZone=0;
        chain.maxCorrection=0.5f; chain.smoothTime=0;
        Ground(0);
    }
    void TearDown() override {
        m_world.BeginSceneSync(); m_world.EndSceneSync(); m_scene.Clear(); EngineFixture::TearDown();
    }
    scene::IKSolverComponent& Solver() { return *m_actor->GetComponent<scene::IKSolverComponent>(); }
    scene::IKChain& Chain() { return Solver().chains[0]; }
    scene::GameObject& Target(const char* name, const math::Vector3& position) {
        auto& target = m_scene.CreateGameObject(name);
        target.transform.position = position;
        return target;
    }
    void ExpectLegLengthsAndDirections(int root, int mid, int tip) {
        const auto upper = m_fk[mid] - m_fk[root];
        const auto lower = m_fk[tip] - m_fk[mid];
        const auto solvedUpper = m_nodes[mid]->transform.worldPosition - m_nodes[root]->transform.worldPosition;
        const auto solvedLower = m_nodes[tip]->transform.worldPosition - m_nodes[mid]->transform.worldPosition;
        EXPECT_NEAR(solvedUpper.Length(), upper.Length(), 1.0e-5f);
        EXPECT_NEAR(solvedLower.Length(), lower.Length(), 1.0e-5f);
        EXPECT_VEC3_NEAR(m_nodes[root]->transform.worldRotation * upper, solvedUpper, 1.0e-5f);
        EXPECT_VEC3_NEAR(m_nodes[mid]->transform.worldRotation * lower, solvedLower, 1.0e-5f);
    }
    void Ground(float y) {
        m_floor.Update({0,y-0.5f,0},math::Quaternion::Identity());
        m_world.BeginSceneSync();
        m_world.SyncCollider({},physics::ColliderInstance{&m_floor,nullptr,&m_material,math::Vector3::ZERO,false,0});
        m_world.EndSceneSync();
    }
    void Step(float dt=1.0f/60) {
        auto& animator=*m_actor->GetComponent<scene::AnimatorComponent>();
        for (size_t i=0;i<m_nodes.size();++i) {
            const int parent=m_model.skeleton->nodes[i].parentIndex;
            m_nodes[i]->transform.position=parent<0 ? m_fk[i] : m_fk[i]-m_fk[parent];
            m_nodes[i]->transform.rotation=math::Quaternion::Identity();
            animator.nodeGlobalTransforms[i]=math::Matrix4::TRS(m_fk[i],math::Quaternion::Identity(),math::Vector3::ONE);
        }
        scene::FlushWorldTransforms(m_scene);
        SystemContext context{m_scene,m_world,&m_resources,nullptr,dt,dt,true,true};
        scene::IKSystem{}.Update(context);
    }
};

TEST_F(FootIKTest, RagdollShapeDeterminesSoleHeightInsteadOfManualAnkleOffset)
{
    auto& ragdoll=m_actor->AddComponent<scene::RagdollComponent>();
    ragdoll.runtime.rig=std::make_unique<scene::RagdollRig>();
    scene::RagdollProfile profile; profile.definitionsOnly=true;
    std::vector<scene::RagdollBonePose> bones;
    for(size_t i=0;i<m_fk.size();++i) {
        scene::RagdollBonePose bone;
        bone.name=m_model.skeleton->nodes[i].name; bone.parent=m_model.skeleton->nodes[i].parentIndex;
        bone.position=m_fk[i]; bones.push_back(bone);
    }
    for(const auto* name:{"LeftFoot","RightFoot"}) {
        scene::RagdollBodyDefinition body; body.bone=name; body.shape=scene::RagdollBodyShape::BOX;
        body.center={0,-0.08f,0}; body.halfExtents={0.05f,0.04f,0.12f}; body.mass=1;
        profile.bodies.push_back(body);
    }
    ragdoll.runtime.rig->Build(bones,profile);
    Chain().footSurfaceOffset=0.3f; Chain().correctionDeadZone=0.1f; Chain().adjustHip=false;
    Step();
    EXPECT_TRUE(Chain().runtimePhysicsFootShape);
    EXPECT_NEAR(m_nodes[3]->transform.worldPosition.y,0.12f,1.0e-5f);
    EXPECT_NEAR(m_nodes[7]->transform.worldPosition.y,0.12f,1.0e-5f);
    math::Quaternion rotation;
    float height=0;
    ASSERT_TRUE(ragdoll.runtime.rig->TryGetFootPlacement("LeftFoot",math::Quaternion::Identity(),{0,0.8f,0.6f},rotation,height));
    EXPECT_NEAR(height,0.15f,1.0e-5f);
    EXPECT_VEC3_NEAR(rotation*math::Vector3::UP,(math::Vector3{0,0.8f,0.6f}),1.0e-5f);
    const auto filteredNormal=math::Vector3::UP;
    ASSERT_TRUE(ragdoll.runtime.rig->TryGetFootPlacement("LeftFoot",math::Quaternion::Identity(),
        {0,0.8f,0.6f},rotation,height,&filteredNormal));
    EXPECT_VEC3_NEAR(rotation*math::Vector3::UP,filteredNormal,1.0e-5f);
    EXPECT_NEAR(height,0.21f,1.0e-5f);
    auto& rig=*ragdoll.runtime.rig;
    rig.SetGravity(math::Vector3::ZERO); rig.SetDrive(true,1,1,1);
    ragdoll.mode=scene::RagdollMode::Active; ragdoll.weight=1;
    rig.GetBody(0)->SetPosition({m_fk[3].x+0.04f,0.04f,m_fk[3].z});
    rig.RefreshContacts(&m_world); rig.Step(1.0f/60);
    Step();
    EXPECT_NEAR(m_nodes[3]->transform.worldPosition.x,m_fk[3].x+0.04f,0.0001f);
    ragdoll.weight=0.5f; Step();
    EXPECT_NEAR(m_nodes[3]->transform.worldPosition.x,m_fk[3].x+0.02f,0.0001f);
    ragdoll.weight=1; Chain().smoothTime=0.1f;
    rig.GetBody(0)->SetPosition({m_fk[3].x+0.12f,0.04f,m_fk[3].z});
    rig.RefreshContacts(&m_world); rig.Step(1.0f/60); Step();
    const float plantedX=m_nodes[3]->transform.worldPosition.x;
    EXPECT_GT(plantedX,m_fk[3].x+0.04f);
    EXPECT_LT(plantedX,m_fk[3].x+0.12f);
    Chain().maxCorrection=0.01f; Step();
    EXPECT_LE(std::abs(m_nodes[3]->transform.worldPosition.x-m_fk[3].x),0.01001f);
    Chain().maxCorrection=0; Step();
    EXPECT_NEAR(m_nodes[3]->transform.worldPosition.x,m_fk[3].x,0.00001f);
    Chain().maxCorrection=0.5f; Step();
    rig.GetBody(0)->SetPosition({m_fk[3].x+0.12f,0.5f,m_fk[3].z});
    rig.RefreshContacts(&m_world); rig.Step(1.0f/60); Step();
    EXPECT_GT(m_nodes[3]->transform.worldPosition.x,m_fk[3].x);
    EXPECT_LT(m_nodes[3]->transform.worldPosition.x,plantedX);
    ragdoll.mode=scene::RagdollMode::Passive; rig.SetDrive(false,1,1,1); Step();
    EXPECT_NEAR(m_nodes[3]->transform.worldPosition.x,m_fk[3].x,0.0001f);
    Chain().smoothTime=0; Chain().useRagdollFootShape=false; Step();
    EXPECT_FALSE(Chain().runtimePhysicsFootShape);
    EXPECT_NEAR(m_nodes[3]->transform.worldPosition.y,0.2f,1.0e-5f);
}
TEST_F(FootIKTest, SwingReleaseFadesPhysicalFootOffsetWithoutSnappingToFk)
{
    auto& ragdoll = m_actor->AddComponent<scene::RagdollComponent>();
    ragdoll.runtime.rig = std::make_unique<scene::RagdollRig>();
    scene::RagdollProfile profile;
    profile.definitionsOnly = true;
    std::vector<scene::RagdollBonePose> bones;
    for (size_t i = 0; i < m_fk.size(); ++i) {
        scene::RagdollBonePose bone;
        bone.name = m_model.skeleton->nodes[i].name;
        bone.parent = m_model.skeleton->nodes[i].parentIndex;
        bone.position = m_fk[i];
        bones.push_back(bone);
    }
    for (const auto* name : {"LeftFoot", "RightFoot"}) {
        scene::RagdollBodyDefinition body;
        body.bone = name;
        body.shape = scene::RagdollBodyShape::BOX;
        body.center = {0, -0.08f, 0};
        body.halfExtents = {0.05f, 0.04f, 0.12f};
        body.mass = 1.0f;
        profile.bodies.push_back(body);
    }
    auto& rig = *ragdoll.runtime.rig;
    rig.Build(bones, profile);
    rig.Capture(bones);
    rig.SetGravity(math::Vector3::ZERO);
    rig.SetDrive(true, 1.0f, 1.0f, 1.0f);
    ragdoll.mode = scene::RagdollMode::Active;
    ragdoll.weight = 1.0f;
    Chain().adjustHip = false;
    Chain().footPlantDistance = 0.06f;
    ASSERT_NE(rig.GetBody(0), nullptr);
    rig.GetBody(0)->SetPosition({m_fk[3].x + 0.04f, 0.04f, m_fk[3].z});
    rig.RefreshContacts(&m_world);
    rig.Step(1.0f / 60.0f);
    Step();
    ASSERT_TRUE(Solver().runtimeLeftFootGrounded);
    ASSERT_NEAR(m_nodes[3]->transform.worldPosition.x - m_fk[3].x, 0.04f, 0.0001f);
    ExpectLegLengthsAndDirections(1, 2, 3);
    const auto plantedFk = m_fk;
    const float upperLength = (m_fk[2] - m_fk[1]).Length();
    const auto expectRelease = [&] {
        EXPECT_FALSE(Solver().runtimeLeftFootGrounded);
        const float firstOffset = m_nodes[3]->transform.worldPosition.x - m_fk[3].x;
        EXPECT_GT(firstOffset, 0.02f);
        EXPECT_LT(firstOffset, 0.04f);
        ExpectLegLengthsAndDirections(1, 2, 3);
        for (int frame = 0; frame < 60; ++frame) {
            Step();
            EXPECT_FALSE(Solver().runtimeLeftFootGrounded);
            ExpectLegLengthsAndDirections(1, 2, 3);
        }
        EXPECT_LT(std::abs(m_nodes[3]->transform.worldPosition.x - m_fk[3].x), 0.0005f);
    };

    Chain().smoothTime = 0.1f;
    m_fk[3].y += 0.091f;
    m_fk[4].y += 0.091f;
    const float halfSpan = (m_fk[1].y - m_fk[3].y) * 0.5f;
    m_fk[2].y = (m_fk[1].y + m_fk[3].y) * 0.5f;
    m_fk[2].z = std::sqrt(upperLength * upperLength - halfSpan * halfSpan);
    Step();
    {
        SCOPED_TRACE("swing with ground ray");
        expectRelease();
    }

    m_fk = plantedFk;
    Chain().smoothTime = 0.0f;
    rig.GetBody(0)->SetPosition({m_fk[3].x + 0.04f, 0.04f, m_fk[3].z});
    rig.RefreshContacts(&m_world);
    rig.Step(1.0f / 60.0f);
    Step();
    ASSERT_TRUE(Solver().runtimeLeftFootGrounded);
    ASSERT_NEAR(m_nodes[3]->transform.worldPosition.x - m_fk[3].x, 0.04f, 0.0001f);
    ExpectLegLengthsAndDirections(1, 2, 3);

    m_world.BeginSceneSync();
    m_world.EndSceneSync();
    rig.RefreshContacts(&m_world);
    Chain().smoothTime = 0.1f;
    Step();
    {
        SCOPED_TRACE("ground ray lost");
        EXPECT_FALSE(Solver().runtimeRightFootGrounded);
        expectRelease();
    }
}

TEST_F(FootIKTest, GroundLossFadesSoleRotationWithoutSnappingToFk)
{
    const float slopeAngle = 0.2f;
    const auto slopeRotation = math::Quaternion::FromAxisAngle(math::Vector3::FORWARD, slopeAngle);
    const auto normal = slopeRotation * math::Vector3::UP;
    m_floor.Update(normal * -0.5f, slopeRotation);
    m_world.BeginSceneSync();
    m_world.SyncCollider({}, physics::ColliderInstance{&m_floor, nullptr, &m_material, math::Vector3::ZERO, false, 0});
    m_world.EndSceneSync();
    Chain().useRagdollFootShape = false;
    Chain().footNormalAxis = math::Vector3::UP;
    Chain().adjustHip = false;
    Step();
    ASSERT_TRUE(Solver().runtimeLeftFootGrounded);
    ASSERT_TRUE(Solver().runtimeRightFootGrounded);
    for (const int foot : {3, 7})
        ASSERT_VEC3_NEAR(m_nodes[foot]->transform.worldRotation * math::Vector3::UP, normal, 1.0e-5f);

    m_world.BeginSceneSync();
    m_world.EndSceneSync();
    Chain().smoothTime = 0.1f;
    Step();
    EXPECT_FALSE(Solver().runtimeLeftFootGrounded);
    EXPECT_FALSE(Solver().runtimeRightFootGrounded);
    for (const int foot : {3, 7}) {
        const auto soleNormal = m_nodes[foot]->transform.worldRotation * math::Vector3::UP;
        const float angle = std::atan2(-soleNormal.x, soleNormal.y);
        EXPECT_GT(angle, 0.12f);
        EXPECT_LT(angle, slopeAngle);
    }
    for (int frame = 0; frame < 60; ++frame) {
        ExpectLegLengthsAndDirections(1, 2, 3);
        ExpectLegLengthsAndDirections(5, 6, 7);
        Step();
    }
    for (const int foot : {3, 7})
        EXPECT_VEC3_NEAR(m_nodes[foot]->transform.worldRotation * math::Vector3::UP, math::Vector3::UP, 1.0e-5f);
}

TEST_F(FootIKTest, AutoPoleCrossesPreferredHemisphereWithoutKneeJump)
{
    m_fk[1] = {0, 1, 0};
    m_fk[3] = math::Vector3::ZERO;
    m_fk[4] = {0, 0, 0.1f};
    Chain().adjustHip = false;
    Chain().useRagdollFootShape = false;
    Chain().footSurfaceOffset = 0.0f;
    Chain().autoPole = true;
    Chain().autoPoleLocalDirection = math::Vector3::FORWARD;
    math::Vector3 previousMid;
    for (const float side : {-0.003f, 0.003f}) {
        m_fk[2] = {0.3f, 0.5f, side};
        Step();
        ASSERT_TRUE(Solver().runtimeLeftFootGrounded);
        ExpectLegLengthsAndDirections(1, 2, 3);
        EXPECT_VEC3_NEAR(m_nodes[3]->transform.worldPosition, m_fk[3], 1.0e-5f);
        const auto solvedMid = m_nodes[2]->transform.worldPosition;
        EXPECT_GE(solvedMid.z, -1.0e-5f);
        if (side > 0.0f) EXPECT_LT((solvedMid - previousMid).Length(), 0.02f);
        previousMid = solvedMid;
    }
    m_fk[2] = {0, 0.5f, 0.3f};
    Step();
    ExpectLegLengthsAndDirections(1, 2, 3);
    EXPECT_VEC3_NEAR(m_nodes[2]->transform.worldPosition, m_fk[2], 1.0e-5f);
}

TEST_F(FootIKTest, UnevenGroundDoesNotMisclassifyTheLowerGroundFootAsSwinging)
{
    const float angle=0.5235987756f;
    const auto rotation=math::Quaternion::FromAxisAngle(math::Vector3::FORWARD,angle);
    const auto normal=rotation*math::Vector3::UP;
    m_floor.Update(normal*-0.5f,rotation);
    m_world.BeginSceneSync();
    m_world.SyncCollider({},physics::ColliderInstance{&m_floor,nullptr,&m_material,math::Vector3::ZERO,false,0});
    m_world.EndSceneSync();
    Step();
    EXPECT_TRUE(Solver().runtimeLeftFootGrounded);
    EXPECT_TRUE(Solver().runtimeRightFootGrounded);
    EXPECT_NEAR(m_nodes[3]->transform.worldPosition.y,0.1f-0.057735f,0.0001f);
    EXPECT_NEAR(m_nodes[7]->transform.worldPosition.y,0.1f+0.057735f,0.0001f);
}
TEST_F(FootIKTest, FlatGroundPreservesFkHipAndLegLengthsWithoutArtificialBendReserve)
{
    Chain().maxExtension=0.5f;
    for(int i=0;i<120;++i) Step();
    EXPECT_NEAR(m_nodes[0]->transform.worldPosition.y,1,1.0e-5f);
    EXPECT_VEC3_NEAR(m_nodes[3]->transform.worldPosition,m_fk[3],1.0e-5f);
    EXPECT_NEAR((m_nodes[2]->transform.worldPosition-m_nodes[1]->transform.worldPosition).Length(),
        (m_fk[2]-m_fk[1]).Length(),1.0e-5f);
    EXPECT_NEAR(Solver().runtimeHipOffset,0,1.0e-5f);
}
TEST_F(FootIKTest, DownwardTerrainCorrectionCannotExceedIndependentHipLimit)
{
    Ground(-0.3f); Chain().maxHipLowering=0.06f;
    Step();
    EXPECT_NEAR(m_nodes[0]->transform.worldPosition.y,0.94f,1.0e-5f);
    EXPECT_TRUE(Chain().runtimeHipLimited);
    EXPECT_NEAR(Solver().runtimeHipOffset,-0.06f,1.0e-5f);
    Chain().hipWeight=0; Step();
    EXPECT_NEAR(m_nodes[0]->transform.worldPosition.y,1,1.0e-5f);
}
TEST_F(FootIKTest, RaisingIsOptInAndRespectsItsOwnLimit)
{
    Ground(0.2f); Step();
    EXPECT_NEAR(m_nodes[0]->transform.worldPosition.y,1,1.0e-5f);
    Chain().maxHipRaising=0.03f; Step();
    EXPECT_NEAR(m_nodes[0]->transform.worldPosition.y,1.03f,1.0e-5f);
}
TEST_F(FootIKTest, ExplicitBendReserveAndWeightRemainInsideTheHipBudget)
{
    Chain().hipBendReserve=0.08f; Chain().maxHipLowering=0.03f; Chain().weight=0.5f;
    Step();
    EXPECT_NEAR(m_nodes[0]->transform.worldPosition.y,0.985f,1.0e-5f);
    EXPECT_TRUE(Chain().runtimeHipLimited);
    Chain().weight=0; Step();
    EXPECT_NEAR(m_nodes[0]->transform.worldPosition.y,1,1.0e-5f);
    EXPECT_NEAR(Chain().runtimeAppliedHip,0,1.0e-5f);
}
TEST_F(FootIKTest, TighteningLimitAndDisablingHipDiscardOutOfRangeHistoryImmediately)
{
    Ground(-0.3f); Chain().smoothTime=0.1f;
    for(int i=0;i<120;++i) Step();
    Chain().maxHipLowering=0.01f; Step();
    EXPECT_GE(m_nodes[0]->transform.worldPosition.y,0.99f-1.0e-5f);
    Chain().adjustHip=false; Step();
    EXPECT_NEAR(m_nodes[0]->transform.worldPosition.y,1,1.0e-5f);
    Solver().enabled=false; Step();
    EXPECT_NEAR(Chain().smoothedLeft,0,1.0e-5f);
    Solver().enabled=true; Ground(0); Step();
    EXPECT_NEAR(m_nodes[0]->transform.worldPosition.y,1,1.0e-5f);
}
TEST_F(FootIKTest, MissingHipAndMissingGroundNeverApplyPhantomHipDisplacement)
{
    Chain().hipBoneName="Missing"; Ground(-0.3f); Step();
    EXPECT_NEAR(m_nodes[0]->transform.worldPosition.y,1,1.0e-5f);
    EXPECT_NEAR(Solver().runtimeHipOffset,0,1.0e-5f);
    Chain().hipBoneName="Hips"; Ground(-5); Step();
    EXPECT_FALSE(Solver().runtimeLeftFootGrounded);
    EXPECT_FALSE(Solver().runtimeRightFootGrounded);
    EXPECT_NEAR(m_nodes[0]->transform.worldPosition.y,1,1.0e-5f);
}
TEST_F(FootIKTest, HipTuningSurvivesSceneRoundTripWithoutPersistingRuntimeState)
{
    Chain().hipWeight=0.4f; Chain().maxHipLowering=0.03f;
    Chain().maxHipRaising=0.02f; Chain().hipBendReserve=0.01f;
    Ground(-0.3f); Step();
    const auto text=scene::SceneSerializer::SaveToText(m_scene);
    auto loaded=scene::SceneSerializer::LoadDataFromText(text,"");
    ASSERT_NE(loaded,nullptr);
    auto* actor=loaded->FindByGuid(m_actor->instanceId); ASSERT_NE(actor,nullptr);
    const auto* solver=actor->GetComponent<scene::IKSolverComponent>(); ASSERT_NE(solver,nullptr);
    ASSERT_EQ(solver->chains.size(),1U);
    const auto& chain=solver->chains[0];
    EXPECT_NEAR(chain.hipWeight,0.4f,1.0e-6f);
    EXPECT_NEAR(chain.maxHipLowering,0.03f,1.0e-6f);
    EXPECT_NEAR(chain.maxHipRaising,0.02f,1.0e-6f);
    EXPECT_NEAR(chain.hipBendReserve,0.01f,1.0e-6f);
    EXPECT_NEAR(chain.runtimeAppliedHip,0,1.0e-6f);
}
TEST_F(FootIKTest, HalfWeightProducesHalfFootCorrectionRatherThanQuarterCorrection)
{
    Chain().adjustHip=false; Chain().weight=0.5f; Ground(0.2f);
    Step();
    EXPECT_NEAR(m_nodes[3]->transform.worldPosition.y,0.2f,1.0e-5f);
}
TEST_F(FootIKTest, InvalidSavedTuningRemainsFiniteAndBoundedAtEvaluation)
{
    Chain().maxHipLowering=-1; Chain().hipWeight=std::numeric_limits<float>::quiet_NaN();
    Chain().smoothTime=-1; Ground(-0.3f); Step();
    EXPECT_NEAR(m_nodes[0]->transform.worldPosition.y,1,1.0e-5f);
    EXPECT_TRUE(std::isfinite(Solver().runtimeHipOffset));
}
TEST_F(FootIKTest, HipCorrectionMovesToeDescendantsExactlyOnce)
{
    for (const float ground : {-0.05f, 0.05f}) {
        Chain().maxHipRaising=0.1f;
        Ground(ground); Step();
        EXPECT_NEAR(Solver().runtimeHipOffset,ground,1.0e-5f);
        EXPECT_VEC3_NEAR(m_nodes[4]->transform.worldPosition-m_nodes[3]->transform.worldPosition,
            m_fk[4]-m_fk[3],1.0e-5f);
        EXPECT_VEC3_NEAR(m_nodes[8]->transform.worldPosition-m_nodes[7]->transform.worldPosition,
            m_fk[8]-m_fk[7],1.0e-5f);
        const auto toePosition=m_nodes[4]->transform.worldPosition;
        scene::FlushWorldTransforms(m_scene);
        EXPECT_VEC3_NEAR(m_nodes[4]->transform.worldPosition,toePosition,1.0e-5f);
    }
}
TEST_F(FootIKTest, FootPlaceThenSpineAndAimDoNotApplyHipCorrectionAgain)
{
    Ground(-0.05f);
    auto& spineTarget=Target("SpineTarget",m_fk[3]+math::Vector3{0,-0.05f,0});
    auto& aimTarget=Target("AimTarget",m_fk[4]+math::Vector3{0,-0.05f,1});
    auto& spine=Solver().chains.emplace_back();
    spine.type=scene::IKSolverType::FABRIK; spine.order=10;
    spine.boneNames={"LeftUpLeg","LeftLeg","LeftFoot"};
    ASSERT_TRUE(scene::SetIKTargetReference(m_scene,spine,spineTarget.GetID()));
    auto& aim=Solver().chains.emplace_back();
    aim.type=scene::IKSolverType::AimAt; aim.order=20; aim.boneNames={"LeftToeBase"};
    ASSERT_TRUE(scene::SetIKTargetReference(m_scene,aim,aimTarget.GetID()));
    Step();
    EXPECT_EQ(Solver().runtimeSolvedChainCount,3);
    for (const int node : {1,2,3,4})
        EXPECT_VEC3_NEAR(m_nodes[node]->transform.worldPosition,m_fk[node]+math::Vector3(0,-0.05f,0),1.0e-5f);
}
TEST_F(FootIKTest, FootWeightsPreserveBothLegLengthsAndRotationDirections)
{
    Chain().adjustHip=false; Ground(0.2f);
    for (const float weight : {0.0f,0.25f,0.5f,0.75f,1.0f}) {
        Chain().weight=weight; Step();
        EXPECT_NEAR(m_nodes[3]->transform.worldPosition.y,m_fk[3].y+0.2f*weight,1.0e-5f);
        ExpectLegLengthsAndDirections(1,2,3);
        ExpectLegLengthsAndDirections(5,6,7);
    }
}
TEST_F(FootIKTest, ReducedFootCorrectionLimitImmediatelyBoundsSmoothedOutput)
{
    Chain().adjustHip=false; Chain().smoothTime=0.1f; Ground(0.2f);
    for (int i=0;i<120;++i) Step();
    EXPECT_GT(m_nodes[3]->transform.worldPosition.y-m_fk[3].y,0.19f);
    Chain().maxCorrection=0.02f; Step();
    EXPECT_LE(m_nodes[3]->transform.worldPosition.y-m_fk[3].y,0.02001f);
    Chain().maxCorrection=0; Step();
    EXPECT_VEC3_NEAR(m_nodes[3]->transform.worldPosition,m_fk[3],1.0e-5f);
}
TEST_F(FootIKTest, TwoBoneAndHandWeightsPreserveLengthAtTheWeightedTargetForEitherPole)
{
    auto& target=Target("LimbTarget",{0.4f,0.6f,0.15f});
    auto& pole=Target("LimbPole",{-0.1f,1,1});
    target.transform.rotation=math::Quaternion::FromAxisAngle(math::Vector3::UP,0.6f);
    for (const auto type : {scene::IKSolverType::TwoBone,scene::IKSolverType::HandPlace}) {
        Chain()={}; Chain().type=type; Chain().boneNames={"LeftUpLeg","LeftLeg","LeftFoot"};
        Chain().softness=0; Chain().maxExtension=1;
        ASSERT_TRUE(scene::SetIKTargetReference(m_scene,Chain(),target.GetID()));
        ASSERT_TRUE(scene::SetIKPoleReference(m_scene,Chain(),pole.GetID()));
        for (const float poleSide : {-1.0f,1.0f}) {
            pole.transform.position.z=poleSide;
            for (const float weight : {0.0f,0.25f,0.5f,0.75f,1.0f}) {
                Chain().weight=weight; Step();
                ExpectLegLengthsAndDirections(1,2,3);
                EXPECT_VEC3_NEAR(m_nodes[3]->transform.worldPosition,
                    math::Vector3::Lerp(m_fk[3],target.transform.worldPosition,weight),1.0e-5f);
                if (type==scene::IKSolverType::HandPlace) {
                    const auto expected=math::Quaternion::Slerp(math::Quaternion::Identity(),
                        target.transform.worldRotation,weight);
                    EXPECT_VEC3_NEAR(m_nodes[3]->transform.worldRotation*math::Vector3::FORWARD,
                        expected*math::Vector3::FORWARD,1.0e-5f);
                }
            }
        }
    }
}
TEST_F(FootIKTest, UnreachableInnerTargetAndFullBodyRotationLimitKeepFixedLengths)
{
    m_fk[1]={0,0,0}; m_fk[2]={1,0,0}; m_fk[3]={1.5f,0,0}; m_fk[4]={1.5f,0,0.1f};
    auto& target=Target("NearTarget",{0.01f,0,0});
    Chain()={}; Chain().type=scene::IKSolverType::TwoBone;
    Chain().boneNames={"LeftUpLeg","LeftLeg","LeftFoot"};
    Chain().softness=0; Chain().maxExtension=1;
    ASSERT_TRUE(scene::SetIKTargetReference(m_scene,Chain(),target.GetID()));
    Step();
    ExpectLegLengthsAndDirections(1,2,3);
    EXPECT_GE((m_nodes[3]->transform.worldPosition-m_nodes[1]->transform.worldPosition).Length(),0.5f);
    target.transform.position={0,1.2f,0}; Chain().weight=0.5f;
    auto& fullBody=Solver().chains.emplace_back();
    fullBody.type=scene::IKSolverType::FullBodyBiped;
    fullBody.fullBodyMaxRotationDegrees=15; fullBody.fullBodyIterations=1;
    Step();
    ExpectLegLengthsAndDirections(1,2,3);
    for (const int node : {1,2}) {
        const float angle=std::acos(math::Clamp(
            math::Vector3::Dot(m_nodes[node]->transform.worldRotation*math::Vector3::RIGHT,
                math::Vector3::RIGHT),-1.0f,1.0f));
        EXPECT_LE(angle,15.0f*math::PI/180.0f+1.0e-5f);
    }
    Solver().chains[1].fullBodyIterations=16; Step();
    ExpectLegLengthsAndDirections(1,2,3);
}
TEST_F(FootIKTest, OppositePoleBlendsContinuouslyThroughHalfWeightWithFixedTipAndLengths)
{
    const float kneeHeight=std::sqrt(0.75f);
    m_fk[1]={0,0,0}; m_fk[2]={0.5f,kneeHeight,0}; m_fk[3]={1,0,0}; m_fk[4]={1,0,0.1f};
    auto& target=Target("FixedTip",m_fk[3]);
    auto& pole=Target("OppositePole",{0.5f,-1,0});
    Chain()={}; Chain().type=scene::IKSolverType::TwoBone;
    Chain().boneNames={"LeftUpLeg","LeftLeg","LeftFoot"};
    Chain().softness=0; Chain().maxExtension=1;
    ASSERT_TRUE(scene::SetIKTargetReference(m_scene,Chain(),target.GetID()));
    ASSERT_TRUE(scene::SetIKPoleReference(m_scene,Chain(),pole.GetID()));
    math::Vector3 previousMid;
    bool hasPrevious=false;
    for (const float weight : {0.49f,0.5f,0.51f}) {
        Chain().weight=weight; Step();
        ExpectLegLengthsAndDirections(1,2,3);
        EXPECT_VEC3_NEAR(m_nodes[3]->transform.worldPosition,m_fk[3],1.0e-5f);
        const auto mid=m_nodes[2]->transform.worldPosition;
        EXPECT_LT(std::abs(mid.y),0.04f);
        EXPECT_GT(std::abs(mid.z),0.8f);
        if (hasPrevious) EXPECT_LT((mid-previousMid).Length(),0.04f);
        previousMid=mid; hasPrevious=true;
    }
    Chain().weight=1; Step();
    EXPECT_VEC3_NEAR(m_nodes[2]->transform.worldPosition,(math::Vector3{0.5f,-kneeHeight,0}),1.0e-5f);
    Chain().weight=0; Step();
    EXPECT_VEC3_NEAR(m_nodes[2]->transform.worldPosition,m_fk[2],1.0e-5f);
}
TEST_F(FootIKTest, FullBodyLookAtAdvancesTheWholeFrameEvenWithoutPositionEffectors)
{
    auto& target=Target("LookTarget",m_fk[0]+math::Vector3{1,0,1});
    Chain()={}; Chain().type=scene::IKSolverType::AimAt; Chain().boneNames={"Hips"};
    Chain().lookAtUpAxis=math::Vector3::ZERO;
    ASSERT_TRUE(scene::SetIKTargetReference(m_scene,Chain(),target.GetID()));
    auto& fullBody=Solver().chains.emplace_back();
    fullBody.type=scene::IKSolverType::FullBodyBiped; fullBody.fullBodyIterations=1;
    Step();
    const auto onePassDirection=m_nodes[0]->transform.worldRotation*math::Vector3::FORWARD;
    Chain().lookAtHasState=false;
    Solver().chains[1].fullBodyIterations=4; Step();
    EXPECT_VEC3_NEAR(m_nodes[0]->transform.worldRotation*math::Vector3::FORWARD,onePassDirection,1.0e-5f);
    EXPECT_EQ(Solver().runtimeFullBodyIterations,1);
}
TEST_F(FootIKTest, FullBodyLookAtWeightAndAngleLimitRemainIndependentOfIterationCount)
{
    auto& target=Target("WeightedLookTarget",m_fk[4]+math::Vector3::RIGHT);
    auto& farTarget=Target("UnreachableTip",{10,0,0});
    Chain()={}; Chain().type=scene::IKSolverType::AimAt; Chain().boneNames={"LeftToeBase"};
    Chain().weight=0.5f; Chain().lookAtUpAxis=math::Vector3::ZERO; Chain().lookAtSpeed=20;
    ASSERT_TRUE(scene::SetIKTargetReference(m_scene,Chain(),target.GetID()));
    auto& limb=Solver().chains.emplace_back();
    limb.type=scene::IKSolverType::TwoBone; limb.boneNames={"RightUpLeg","RightLeg","RightFoot"};
    ASSERT_TRUE(scene::SetIKTargetReference(m_scene,limb,farTarget.GetID()));
    Solver().chains.emplace_back().type=scene::IKSolverType::FullBodyBiped;
    for (const float clampDegrees : {30.0f,90.0f}) {
        for (const int iterations : {1,4,16}) {
            SCOPED_TRACE(iterations);
            Chain().lookAtHasState=false;
            Chain().lookAtClampAngle=clampDegrees;
            Solver().chains[2].fullBodyIterations=iterations;
            for (int frame=0;frame<120;++frame) Step();
            const auto expected=math::Quaternion::FromAxisAngle(math::Vector3::UP,
                clampDegrees*0.5f*math::PI/180.0f)*math::Vector3::FORWARD;
            EXPECT_VEC3_NEAR(m_nodes[4]->transform.worldRotation*math::Vector3::FORWARD,expected,1.0e-4f);
            EXPECT_EQ(Solver().runtimeFullBodyIterations,iterations);
        }
    }
}
} /// @note namespace fbzz::tests
