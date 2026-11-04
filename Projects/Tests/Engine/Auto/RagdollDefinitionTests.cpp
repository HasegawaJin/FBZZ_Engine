/// @file    RagdollDefinitionTests.cpp
/// @brief   明示物理設定の保存・検証・形状生成と駆動切替を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-03
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Ragdoll/RagdollDefinition.hpp>
#include <Engine/Scene/Ragdoll/RagdollRig.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Physics/Collider.hpp>
#include <Physics/OBBCollider.hpp>
#include <Physics/HeightFieldCollider.hpp>
#include <Physics/World.hpp>
#include <limits>

namespace fbzz::tests {
class RagdollDefinitionTest : public testkit::EngineFixture {};

TEST_F(RagdollDefinitionTest, RoundTripRetainsShapesMassJointFramesExclusionsAndDrive)
{
    testkit::TempDir temp("ragdoll");
    ASSERT_TRUE(temp.IsValid());
    scene::RagdollDefinition value;
    value.name="Test";
    for (int i=0;i<3;++i) {
        scene::RagdollBodyDefinition b;
        b.bone="Body"+std::to_string(i);
        b.shape=static_cast<scene::RagdollBodyShape>(i);
        b.mass=3.0f+i; b.center={0.1f,0.2f,0.3f}; b.inertia={1,2,3};
        b.jointAnchor={0,0.1f,0};
        b.settings.limits.enabled=true; b.settings.limits.twistMin=-0.5f; b.settings.limits.twistMax=0.8f;
        b.drive.enabled=true; b.drive.space=scene::RagdollDriveSpace::WORLD;
        b.drive.linearFrequency=6; b.drive.maxForce=500;
        value.bodies.push_back(b);
    }
    value.collisionExclusions.push_back({"Body0","Body2"});
    const auto path=temp.File("Test.ragdoll").string();
    std::string error;
    ASSERT_TRUE(scene::SaveRagdollDefinition(path,value,&error)) << error;
    scene::RagdollDefinition loaded;
    ASSERT_TRUE(scene::LoadRagdollDefinition(path,loaded,&error)) << error;
    ASSERT_EQ(loaded.bodies.size(),3U);
    EXPECT_NE(loaded.signature,0U);
    EXPECT_EQ(loaded.collisionExclusions[0].bodyB,"Body2");
    for (int i=0;i<3;++i) {
        EXPECT_EQ(loaded.bodies[i].shape,value.bodies[i].shape);
        EXPECT_NEAR(loaded.bodies[i].mass,value.bodies[i].mass,1.0e-6f);
        EXPECT_VEC3_NEAR(loaded.bodies[i].inertia,value.bodies[i].inertia,1.0e-6f);
        EXPECT_NEAR(loaded.bodies[i].settings.limits.twistMin,-0.5f,1.0e-6f);
        EXPECT_EQ(loaded.bodies[i].drive.space,scene::RagdollDriveSpace::WORLD);
        EXPECT_NEAR(loaded.bodies[i].drive.maxForce,500,1.0e-6f);
    }
    ASSERT_TRUE(util::FileSystem::WriteTextAtomic(path,"[ragdoll]\nversion=99\n"));
    EXPECT_FALSE(scene::LoadRagdollDefinition(path,loaded,&error));
    EXPECT_EQ(loaded.name,"Test");
    EXPECT_EQ(loaded.bodies.size(),3U);
}

TEST_F(RagdollDefinitionTest, InvalidDefinitionsAreRejectedBeforeMutation)
{
    scene::RagdollDefinition value;
    scene::RagdollBodyDefinition b; b.bone="Hips"; value.bodies.push_back(b);
    EXPECT_TRUE(scene::ValidateRagdollDefinition(value));
    value.bodies[0].radius=std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(scene::ValidateRagdollDefinition(value));
    value.bodies[0]=b; value.bodies[0].inertia={1,0,1};
    EXPECT_FALSE(scene::ValidateRagdollDefinition(value));
    value.bodies[0]=b; value.bodies.push_back(b);
    EXPECT_FALSE(scene::ValidateRagdollDefinition(value));
    value.bodies.pop_back(); value.collisionExclusions.push_back({"Hips","Absent"});
    EXPECT_FALSE(scene::ValidateRagdollDefinition(value));
}

TEST_F(RagdollDefinitionTest, ExplicitLeafShapesAndInertiaDriveActualRigidBodies)
{
    std::vector<scene::RagdollBonePose> bones;
    scene::RagdollProfile profile; profile.definitionsOnly=true;
    for (int i=0;i<3;++i) {
        scene::RagdollBonePose bone; bone.name="Body"+std::to_string(i); bone.position={static_cast<float>(i),1,0};
        bones.push_back(bone);
        scene::RagdollBodyDefinition b; b.bone=bone.name;
        b.shape=static_cast<scene::RagdollBodyShape>(i); b.mass=2; b.inertia={1,2,4};
        b.center={0,0.1f,0}; profile.bodies.push_back(b);
    }
    scene::RagdollRig rig; rig.Build(bones,profile); rig.Capture(bones);
    ASSERT_EQ(rig.GetBodyCount(),3);
    const physics::ColliderType types[]={physics::ColliderType::CAPSULE,physics::ColliderType::SPHERE,physics::ColliderType::OBB};
    for (int i=0;i<3;++i) {
        auto* body=rig.BodyOfBone(i);
        ASSERT_NE(body,nullptr);
        ASSERT_NE(body->GetInertiaCollider(),nullptr);
        EXPECT_EQ(body->GetInertiaCollider()->GetType(),types[i]);
        EXPECT_NEAR(body->GetPosition().y,1.1f,1.0e-6f);
        body->ApplyAngularImpulse({0,0,4});
        EXPECT_NEAR(body->GetAngularVelocity().z,1,1.0e-6f);
    }
}

TEST_F(RagdollDefinitionTest, AQuickLightLimbDoesNotSleepWithASupportedHeavyRoot)
{
    scene::RagdollProfile profile; profile.definitionsOnly=true;
    scene::RagdollBodyDefinition root; root.bone="Base"; root.shape=scene::RagdollBodyShape::SPHERE; root.radius=0.1f; root.mass=100;
    auto light=root; light.bone="Hand"; light.radius=0.02f; light.mass=0.01f;
    profile.bodies={root,light};
    std::vector<scene::RagdollBonePose> bones(2);
    bones[0].name="Base"; bones[0].position={0,0.1f,0};
    bones[1].name="Hand"; bones[1].position={0,2,0};
    scene::RagdollRig rig; rig.Build(bones,profile); rig.Capture(bones);
    rig.SetGravity(math::Vector3::ZERO); rig.SetDrag(0,0); rig.SetDrive(false,1,1,1);
    rig.SetGround(0,0); rig.GetBody(0)->m_isStatic=true; rig.GetBody(1)->SetVelocity({0.2f,0,0});
    for (int i=0;i<120;++i) rig.Step(1.0f/120);
    EXPECT_GT(rig.GetBody(1)->GetPosition().x,0.19f);
}

TEST_F(RagdollDefinitionTest, BoxRestsOnTerrainWithMultipleSupportsWithoutRestLock)
{
    for (int substeps:{12,24}) {
        SCOPED_TRACE(substeps);
        physics::HeightFieldCollider terrain(std::vector<float>(81,0.0f),9,9,1,1);
        terrain.Update({-4,0,-4},math::Quaternion::Identity());
        physics::PhysicsMaterial material; material.staticFriction=0.8f; material.dynamicFriction=0.6f; material.restitution=0;
        physics::World world;
        world.BeginSceneSync();
        world.SyncCollider({},physics::ColliderInstance{&terrain,nullptr,&material,math::Vector3::ZERO,false,0});
        world.EndSceneSync();
        scene::RagdollProfile profile; profile.definitionsOnly=true;
        scene::RagdollBodyDefinition box; box.bone="Pelvis"; box.shape=scene::RagdollBodyShape::BOX;
        box.halfExtents={0.3f,0.05f,0.1f}; box.mass=4; profile.bodies.push_back(box);
        std::vector<scene::RagdollBonePose> bones(1); bones[0].name="Pelvis"; bones[0].position={0,2,0};
        bones[0].rotation=math::Quaternion::FromAxisAngle({0,0,1},0.3f);
        scene::RagdollRig rig; rig.Build(bones,profile); rig.Capture(bones); rig.SetDrive(false,1,1,1);
        scene::RagdollContactSettings contacts; contacts.allowRest=false; contacts.surface=material;
        rig.SetSubsteps(substeps);
        rig.SetContactSettings(contacts);
        math::Vector3 settled;
        for (int i=0;i<600;++i) {
            rig.RefreshContacts(&world); rig.Step(1.0f/60); rig.ApplyContactReactions();
            ASSERT_GE(rig.GetBody(0)->GetPosition().y,0.0f) << "frame=" << i << " contacts=" << rig.GetContactCount();
            if (i==539) settled=rig.GetBody(0)->GetPosition();
        }
        EXPECT_GE(rig.GetContactCount(),3);
        EXPECT_NEAR(rig.GetBody(0)->GetPosition().y,0.05f,0.005f);
        EXPECT_LT((rig.GetBody(0)->GetPosition()-settled).Length(),0.001f);
        EXPECT_LT(rig.GetBody(0)->GetAngularVelocity().Length(),0.02f);
    }
}

TEST_F(RagdollDefinitionTest, ContinuousWorldContactsHoldStaticLoadAndSlipAboveFrictionLimit)
{
    for (float horizontal : {1.0f,8.0f}) {
        physics::OBBCollider floor({100,0.5f,100});
        floor.Update({0,-0.5f,0},math::Quaternion::Identity());
        physics::PhysicsMaterial material;
        material.staticFriction=0.5f; material.dynamicFriction=0;
        material.restitution=0;
        physics::World world;
        world.BeginSceneSync();
        world.SyncCollider({},physics::ColliderInstance{&floor,nullptr,&material,math::Vector3::ZERO,false,0});
        world.EndSceneSync();
        scene::RagdollBodyDefinition body;
        body.bone="Foot"; body.shape=scene::RagdollBodyShape::SPHERE; body.radius=0.1f; body.mass=1;
        scene::RagdollProfile profile; profile.definitionsOnly=true; profile.bodies.push_back(body);
        std::vector<scene::RagdollBonePose> bones(1); bones[0].name="Foot"; bones[0].position={0,0.1f,0};
        scene::RagdollRig rig; rig.Build(bones,profile); rig.Capture(bones);
        rig.SetDrive(false,1,1,1); rig.SetGravity({horizontal,-9.81f,0});
        rig.BodyOfBone(0)->SetFreezeRotation({true,true,true});
        scene::RagdollContactSettings contacts;
        contacts.surface=material; contacts.allowRest=false; contacts.self=false;
        rig.SetContactSettings(contacts);
        for (int i=0;i<240;++i) {
            rig.RefreshContacts(&world);
            rig.Step(1.0f/120);
            rig.ApplyContactReactions();
        }
        if (horizontal<2) EXPECT_LT(std::abs(rig.BodyOfBone(0)->GetPosition().x),0.002f);
        else EXPECT_GT(rig.BodyOfBone(0)->GetPosition().x,1.0f);
        EXPECT_NEAR(rig.BodyOfBone(0)->GetPosition().y,0.1f,0.002f);
        EXPECT_TRUE(rig.BodyOfBone(0)->IsFiniteState());
        /// @note 支持を離した後に別の場所で再接触しても、過去の固着点へ戻らない。
        rig.BodyOfBone(0)->SetPosition({-3,2,0});
        rig.BodyOfBone(0)->SetVelocity(math::Vector3::ZERO);
        rig.RefreshContacts(nullptr); rig.Step(1.0f/120);
        rig.BodyOfBone(0)->SetPosition({-3,0.099f,0});
        rig.BodyOfBone(0)->SetVelocity(math::Vector3::ZERO);
        rig.RefreshContacts(&world); rig.Step(1.0f/120); rig.ApplyContactReactions();
        EXPECT_NEAR(rig.BodyOfBone(0)->GetPosition().x,-3,0.01f);
    }
}

TEST_F(RagdollDefinitionTest, PerBodyDriveWorksWithoutGuardAndPassiveDisablesIt)
{
    scene::RagdollBodyDefinition body;
    body.bone="Hips"; body.shape=scene::RagdollBodyShape::SPHERE; body.mass=2;
    body.drive.enabled=true; body.drive.space=scene::RagdollDriveSpace::WORLD;
    body.drive.linearFrequency=5; body.drive.maxForce=1000; body.drive.angularFrequency=5;
    scene::RagdollProfile profile; profile.definitionsOnly=true; profile.bodies.push_back(body);
    std::vector<scene::RagdollBonePose> bones(1); bones[0].name="Hips"; bones[0].position={0,2,0};
    scene::RagdollRig rig; rig.Build(bones,profile); rig.Capture(bones); rig.UpdateDriveTargets(bones);
    rig.SetDrive(true,1,1,1); rig.SetStandingGuard(false,0,0);
    for (int i=0;i<240;++i) rig.Step(1.0f/120);
    EXPECT_NEAR(rig.BodyOfBone(0)->GetPosition().y,2,0.02f);
    auto disabled=body.drive; disabled.enabled=false;
    EXPECT_TRUE(rig.SetBodyDrive("Hips",disabled));
    EXPECT_FALSE(rig.SetBodyDrive("Missing",body.drive));
    rig.Step(0.1f);
    EXPECT_LT(rig.BodyOfBone(0)->GetPosition().y,1.98f);
    EXPECT_TRUE(rig.ClearBodyDrive("Hips"));
    for (int i=0;i<240;++i) rig.Step(1.0f/120);
    EXPECT_NEAR(rig.BodyOfBone(0)->GetPosition().y,2,0.02f);
    rig.SetDrive(false,1,1,1);
    rig.Step(0.1f);
    EXPECT_LT(rig.BodyOfBone(0)->GetPosition().y,1.98f);
}
}
