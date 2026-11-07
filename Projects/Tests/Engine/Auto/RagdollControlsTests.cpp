/// @file    RagdollControlsTests.cpp
/// @brief   部位目標・外力積分・支持観測・独立把持を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-03
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Ragdoll/RagdollRig.hpp>
#include <Engine/Scene/Ragdoll/RagdollControls.hpp>
#include <Physics/World.hpp>
#include <Physics/OBBCollider.hpp>
#include <limits>
#include <algorithm>
#include <cmath>
namespace fbzz::tests {
class RagdollControlsTest:public testkit::EngineFixture {
protected:
    scene::RagdollRig m_rig;
    std::vector<scene::RagdollBonePose> m_bones;
    void Build(int count=1) {
        scene::RagdollProfile profile;profile.definitionsOnly=true;
        for(int i=0;i<count;++i) {
            scene::RagdollBonePose b;b.name="Body"+std::to_string(i);b.position={static_cast<float>(i)*2,1,0};m_bones.push_back(b);
            scene::RagdollBodyDefinition d;d.bone=b.name;d.mass=2;d.inertia={1,1,1};d.shape=scene::RagdollBodyShape::BOX;d.halfExtents={0.2f,1,0.2f};
            d.drive.enabled=true;d.drive.space=scene::RagdollDriveSpace::WORLD;d.drive.linearFrequency=4;d.drive.maxForce=1000;
            profile.bodies.push_back(d);
        }
        m_rig.Build(m_bones,profile);m_rig.Capture(m_bones);m_rig.SetGravity(math::Vector3::ZERO);m_rig.SetDrive(false,1,1,1);
    }
};
TEST_F(RagdollControlsTest, WorldCollisionDoesNotRepeatTheImpactOnALightBodyAcrossSubsteps)
{
    for (float mass:{0.002f,0.2f,2.0f,200.0f,2000.0f}) for (int steps:{1,6,12,24,48}) {
        scene::RagdollProfile profile; profile.definitionsOnly=true;
        scene::RagdollBodyDefinition definition; definition.bone="Body"; definition.mass=2;
        definition.shape=scene::RagdollBodyShape::SPHERE; definition.radius=0.1f; definition.inertia={1,1,1};
        profile.bodies.push_back(definition);
        std::vector<scene::RagdollBonePose> bones(1); bones[0].name="Body"; bones[0].position={0,1,0};
        m_rig.Build(bones,profile); m_rig.Capture(bones); m_rig.SetDrive(false,1,1,1);
        m_rig.SetGravity(math::Vector3::ZERO); m_rig.SetDrag(0,0); m_rig.SetSubsteps(steps);
        m_rig.GetBody(0)->SetVelocity({1,0,0});
        physics::RigidBody other; other.SetMass(mass); other.SetPosition({0.195f,1,0}); other.m_useGravity=false;
        physics::SphereCollider collider(0.1f); collider.Update(other.GetPosition(),math::Quaternion::Identity());
        physics::PhysicsMaterial material; material.restitution=0;
        physics::World world; world.BeginSceneSync(); world.SyncBody({},&other);
        world.SyncCollider({},physics::ColliderInstance{&collider,&other,&material,math::Vector3::ZERO,false,0});
        world.EndSceneSync();
        m_rig.RefreshContacts(&world); m_rig.Step(1.0f/60); m_rig.ApplyContactReactions();
        const auto velocity=m_rig.GetBody(0)->GetVelocity();
        EXPECT_NEAR(2*velocity.x+mass*other.GetVelocity().x,2,0.002f);
        EXPECT_LE(velocity.LengthSq()+0.5f*mass*other.GetVelocity().LengthSq(),1.002f);
        EXPECT_VEC3_NEAR(other.GetPosition(),(math::Vector3{0.195f,1,0}),testkit::kTolerance);
    }
}
TEST_F(RagdollControlsTest, EnumeratesPhysicalComAndAppliesPointImpulse)
{
    Build();auto& c=m_rig.Controls();
    EXPECT_EQ(c.BodyName(0),"Body0");EXPECT_TRUE(c.BodyName(1).empty());EXPECT_FALSE(c.BodyState("Absent").valid);
    auto before=c.BodyState("Body0");ASSERT_TRUE(before.valid);EXPECT_NEAR(before.mass,2,1.0e-6f);
    auto point=before.position+math::Vector3{0,1,0};
    ASSERT_TRUE(c.Impulse("Body0",{2,0,0},&point));
    auto after=c.BodyState("Body0");EXPECT_NEAR(after.velocity.x,1,1.0e-6f);EXPECT_NEAR(after.angularVelocity.z,-2,1.0e-6f);
}
TEST_F(RagdollControlsTest, FiniteDurationForceIsIndependentOfSubstepsAndExpires)
{
    for(int steps:{1,4,12}) {
        m_bones.clear();Build();m_rig.SetSubsteps(steps);
        ASSERT_TRUE(m_rig.Controls().Force("Body0",{10,0,0},0.025f));
        for(int i=0;i<12;++i)m_rig.Step(0.01f);
        EXPECT_NEAR(m_rig.Controls().BodyState("Body0").velocity.x,0.125f,0.001f);
        EXPECT_FALSE(m_rig.Controls().Force("Body0",{1,0,0},-1));
        EXPECT_FALSE(m_rig.Controls().Force("Missing",{1,0,0},1));
    }
}
TEST_F(RagdollControlsTest, ContinuousTorqueAndOffCenterForceProduceAngularImpulse)
{
    Build();auto& c=m_rig.Controls();
    ASSERT_TRUE(c.Torque("Body0",{0,0,2},0.05f));
    for(int i=0;i<10;++i)m_rig.Step(0.01f);
    EXPECT_NEAR(c.BodyState("Body0").angularVelocity.z,0.1f,0.002f);
    m_rig.Capture(m_bones);
    const auto point=c.BodyState("Body0").position+math::Vector3::UP;
    ASSERT_TRUE(c.Force("Body0",{2,0,0},0.05f,&point));
    for(int i=0;i<5;++i)m_rig.Step(0.01f);
    EXPECT_NEAR(c.BodyState("Body0").velocity.x,0.05f,0.002f);
    EXPECT_LT(c.BodyState("Body0").angularVelocity.z,-0.09f);
}

TEST_F(RagdollControlsTest, ManualTargetMovesBodyAndClearRestoresAnimation)
{
    Build();m_rig.SetDrive(true,1,1,1);
    scene::RagdollBodyTarget target;target.position={0.5f,1,0};
    ASSERT_TRUE(m_rig.Controls().SetTarget("Body0",target));
    target.position.x=std::numeric_limits<float>::quiet_NaN();EXPECT_FALSE(m_rig.Controls().SetTarget("Body0",target));
    for(int i=0;i<240;++i){m_rig.UpdateDriveTargets(m_bones,1.0f/120);m_rig.Step(1.0f/120);}
    EXPECT_NEAR(m_rig.Controls().BodyState("Body0").position.x,0.5f,0.01f);
    ASSERT_TRUE(m_rig.Controls().ClearTarget("Body0"));
    for(int i=0;i<240;++i){m_rig.UpdateDriveTargets(m_bones,1.0f/120);m_rig.Step(1.0f/120);}
    EXPECT_NEAR(m_rig.Controls().BodyState("Body0").position.x,0,0.01f);
}

TEST_F(RagdollControlsTest, PreservesGuardRootTransportOnTheFirstPositiveTargetSample)
{
    Build(2);
    m_rig.SetDrag(0, 0);
    m_rig.SetSubsteps(12);
    m_rig.SetStandingGuard(true, 0.01f, 0.01f);
    for (auto& bone : m_bones) bone.position += math::Vector3::RIGHT;

    m_rig.UpdateDriveTargets(m_bones, 1.0f / 30.0f);
    m_rig.Step(1.0f / 120.0f);

    for (const auto& bone : m_bones) {
        SCOPED_TRACE(bone.name);
        const auto state = m_rig.Controls().BodyState(bone.name);
        ASSERT_TRUE(state.valid);
        EXPECT_VEC3_NEAR(state.position, bone.position, testkit::kTolerance);
        EXPECT_VEC3_NEAR(state.velocity, math::Vector3::ZERO, testkit::kTolerance);
    }
}

TEST_F(RagdollControlsTest, PreservesGuardRootTransportWhenTargetSamplesResumeAfterStaticSteps)
{
    Build(2);
    m_rig.SetDrag(0, 0);
    m_rig.SetSubsteps(12);
    m_rig.SetStandingGuard(true, 0.01f, 0.01f);
    m_rig.UpdateDriveTargets(m_bones);
    for (int frame = 0; frame < 120; ++frame) m_rig.Step(1.0f / 120.0f);
    for (auto& bone : m_bones) bone.position += math::Vector3::RIGHT;

    m_rig.UpdateDriveTargets(m_bones, 1.0f / 30.0f);
    m_rig.Step(1.0f / 120.0f);

    for (const auto& bone : m_bones) {
        SCOPED_TRACE(bone.name);
        const auto state = m_rig.Controls().BodyState(bone.name);
        ASSERT_TRUE(state.valid);
        EXPECT_VEC3_NEAR(state.position, bone.position, testkit::kTolerance);
        EXPECT_VEC3_NEAR(state.velocity, math::Vector3::ZERO, testkit::kTolerance);
    }
}

TEST_F(RagdollControlsTest, PreservesCircularWorldDriveMotionAcrossAnimationSampleRates)
{
    scene::RagdollProfile profile;
    profile.definitionsOnly = true;
    scene::RagdollBodyDefinition definition;
    definition.bone = "Child";
    definition.shape = scene::RagdollBodyShape::SPHERE;
    definition.radius = 0.1f;
    definition.mass = 1.0f;
    definition.drive.enabled = true;
    definition.drive.space = scene::RagdollDriveSpace::WORLD;
    definition.drive.linearFrequency = 36.0f;
    definition.drive.angularFrequency = 0.0f;
    definition.drive.maxForce = 1000.0f;
    definition.drive.maxTorque = 0.0f;
    profile.bodies.push_back(definition);
    std::vector<scene::RagdollBodyState> baseline;
    baseline.reserve(240);
    float maximumPositionDifference = 0.0f;
    float maximumVelocityDifference = 0.0f;

    for (const int fps : {60, 120}) {
        SCOPED_TRACE(fps);
        std::vector<scene::RagdollBonePose> bones(2);
        bones[0].name = "Root";
        bones[1].name = "Child";
        bones[1].parent = 0;
        bones[1].position = math::Vector3::RIGHT;
        m_rig.Build(bones, profile);
        ASSERT_EQ(m_rig.GetBodyCount(), 1);
        EXPECT_EQ(m_rig.BodyIndexOfBone(0), -1);
        m_rig.SetGravity(math::Vector3::ZERO);
        m_rig.SetDrag(0, 0);
        m_rig.SetSubsteps(12);
        m_rig.SetRootAnchor(0, 0, 0);
        m_rig.SetStandingGuard(false, 0, 0);
        m_rig.DisableGround();
        m_rig.SetDrive(true, 1, 1, 1);
        m_rig.Capture(bones);
        m_rig.UpdateDriveTargets(bones);
        ASSERT_TRUE(m_rig.Controls().Impulse("Child", {0, 3, 0}));
        const int ticksPerSample = 120 / fps;
        const float sampleDt = 1.0f / static_cast<float>(fps);

        for (int tick = 0; tick < 300; ++tick) {
            if (tick % ticksPerSample == 0) {
                const float targetTime = static_cast<float>(tick / ticksPerSample + 1) * sampleDt;
                const auto rotation = math::Quaternion::FromAxisAngle(math::Vector3::FORWARD, 3.0f * targetTime);
                bones[0].rotation = rotation;
                bones[1].rotation = rotation;
                bones[1].position = rotation * math::Vector3::RIGHT;
                m_rig.UpdateDriveTargets(bones, sampleDt);
            }
            m_rig.Step(1.0f / 120.0f);
            const auto state = m_rig.Controls().BodyState("Child");
            ASSERT_TRUE(state.valid);
            if (tick < 60) continue;
            if (fps == 60) baseline.push_back(state);
            else {
                const auto& expected = baseline[static_cast<std::size_t>(tick - 60)];
                maximumPositionDifference = std::max(maximumPositionDifference, (state.position - expected.position).Length());
                maximumVelocityDifference = std::max(maximumVelocityDifference, (state.velocity - expected.velocity).Length());
            }
        }
    }

    EXPECT_LT(maximumPositionDifference, 0.001f);
    EXPECT_LT(maximumVelocityDifference, 0.003f);
}

TEST_F(RagdollControlsTest, GripsRemainIndependentAndRebuildDropsRuntimeControls)
{
    Build(2);auto& c=m_rig.Controls();physics::XPBDGripSettings s;s.breakForce=0;
    ASSERT_TRUE(c.Grab("Body0",nullptr,c.BodyState("Body0").position,s));
    ASSERT_TRUE(c.Grab("Body1",nullptr,c.BodyState("Body1").position,s));
    m_rig.RefreshContacts(nullptr);m_rig.Step(0.01f);
    ASSERT_TRUE(c.Release("Body0"));
    EXPECT_EQ(c.GrabState("Body0").status,physics::XPBDGripStatus::RELEASED);
    EXPECT_EQ(c.GrabState("Body1").status,physics::XPBDGripStatus::HOLDING);
    EXPECT_TRUE(c.HasGrabs());c.ReleaseAll();EXPECT_FALSE(c.HasGrabs());
    m_rig.Clear();EXPECT_EQ(c.GrabState("Body1").status,physics::XPBDGripStatus::NONE);
}
TEST_F(RagdollControlsTest, SupportMarginsAndCapturePointRespondToVelocity)
{
    Build();m_rig.SetGravity({0,-9.81f,0});
    physics::World world;physics::OBBCollider floor({5,0.5f,5});floor.Update({0,-0.5f,0},math::Quaternion::Identity());
    physics::PhysicsMaterial material;
    world.BeginSceneSync();world.SyncCollider({},physics::ColliderInstance{&floor,nullptr,&material,math::Vector3::ZERO,false,0});world.EndSceneSync();
    for(int i=0;i<120;++i){m_rig.RefreshContacts(&world);m_rig.Step(1.0f/120);}
    const auto balance=m_rig.Controls().Balance();
    ASSERT_TRUE(balance.valid);ASSERT_TRUE(balance.hasSupport);ASSERT_TRUE(balance.captureValid);
    EXPECT_GT(balance.centerMargin,0.1f);EXPECT_GT(balance.captureMargin,0.1f);
    ASSERT_TRUE(m_rig.Controls().Impulse("Body0",{10,0,0}));
    const auto moving=m_rig.Controls().Balance();
    EXPECT_LT(moving.captureMargin,0);EXPECT_GT(moving.capturePoint.x,balance.capturePoint.x+1);
    auto contact=m_rig.Controls().Contact("Body0",0);EXPECT_TRUE(contact.valid);EXPECT_FALSE(contact.self);
    m_rig.SetGravity(math::Vector3::ZERO);EXPECT_FALSE(m_rig.Controls().Balance().captureValid);
    EXPECT_FALSE(m_rig.Controls().Balance(-1).valid);
    world.BeginSceneSync();world.EndSceneSync();
}

TEST_F(RagdollControlsTest, TranslatingAndRotatingSupportUsesRelativeCaptureVelocity)
{
    scene::RagdollProfile profile;
    profile.definitionsOnly = true;
    scene::RagdollBodyDefinition definition;
    definition.bone = "Body";
    definition.shape = scene::RagdollBodyShape::SPHERE;
    definition.radius = 1.0f;
    definition.mass = 2.0f;
    profile.bodies.push_back(definition);
    std::vector<scene::RagdollBonePose> bones(1);
    bones[0].name = "Body";
    bones[0].position = {0, 1, 0};
    m_rig.Build(bones, profile);
    m_rig.Capture(bones);
    m_rig.SetDrive(false, 1, 1, 1);
    m_rig.SetGravity(math::Vector3::ZERO);
    m_rig.SetSubsteps(1);

    physics::RigidBody platform;
    platform.SetMass(100.0f);
    platform.SetPosition({2, -0.49f, 0});
    platform.m_useGravity = false;
    physics::OBBCollider floor({5, 0.5f, 5});
    platform.SetInertiaFromCollider(&floor);
    floor.Update(platform.GetPosition(), math::Quaternion::Identity());
    physics::PhysicsMaterial material;
    physics::World world;
    world.BeginSceneSync();
    world.SyncBody({}, &platform);
    world.SyncCollider({}, physics::ColliderInstance{&floor, &platform, &material, math::Vector3::ZERO, false, 0});
    world.EndSceneSync();
    m_rig.RefreshContacts(&world);
    m_rig.Step(1.0f / 120.0f);
    m_rig.ApplyContactReactions();
    m_rig.SetGravity({0, -9.81f, 0});
    auto& controls = m_rig.Controls();

    for (float yawSpeed : {0.0f, 2.0f}) {
        SCOPED_TRACE(yawSpeed);
        platform.SetVelocity({1, 0.5f, 3});
        platform.SetAngularVelocity({0, yawSpeed, 0});
        const math::Vector3 expectedSurfaceVelocity{1, 0.5f, 3 + 2 * yawSpeed};
        const auto state = controls.BodyState("Body");
        ASSERT_TRUE(state.valid);
        ASSERT_TRUE(controls.Impulse("Body", (expectedSurfaceVelocity - state.velocity) * state.mass));

        const auto contact = controls.Contact("Body", 0);
        ASSERT_TRUE(contact.valid);
        EXPECT_TRUE(contact.dynamic);
        EXPECT_FALSE(contact.self);
        EXPECT_VEC3_NEAR(contact.otherVelocity, expectedSurfaceVelocity, testkit::kTolerance);
        const auto balance = controls.Balance();
        ASSERT_TRUE(balance.hasSupport);
        ASSERT_TRUE(balance.captureValid);
        EXPECT_VEC3_NEAR(balance.supportVelocity, expectedSurfaceVelocity, testkit::kTolerance);
        const math::Vector3 projectedCenter{balance.center.x, balance.supportPoint.y, balance.center.z};
        EXPECT_VEC3_NEAR(balance.capturePoint, projectedCenter, testkit::kTolerance);

        ASSERT_TRUE(controls.Impulse("Body", math::Vector3{2, 0, -1} * state.mass));
        const auto drifting = controls.Balance();
        const float captureSeconds = std::sqrt(balance.height / 9.81f);
        EXPECT_VEC3_NEAR(drifting.capturePoint,
            projectedCenter + math::Vector3(2, 0, -1) * captureSeconds, testkit::kTolerance);
    }

    world.BeginSceneSync();
    world.EndSceneSync();
}

TEST_F(RagdollControlsTest, BoxContactsPreserveTheSupportPolygonFromNarrowPhase)
{
    Build(); m_rig.SetSubsteps(1);
    physics::World world; physics::OBBCollider floor({5,0.5f,5});
    floor.Update({0,-0.49f,0},math::Quaternion::Identity());
    physics::PhysicsMaterial material;
    world.BeginSceneSync();
    world.SyncCollider({},physics::ColliderInstance{&floor,nullptr,&material,math::Vector3::ZERO,false,0});
    world.EndSceneSync();
    m_rig.RefreshContacts(&world); m_rig.Step(1.0f/60);
    const auto state=m_rig.Controls().BodyState("Body0");
    bool corner=false;
    for(int i=0;i<state.contacts;++i) {
        const auto contact=m_rig.Controls().Contact("Body0",i);
        corner=corner || (std::abs(contact.point.x)>0.1999f && std::abs(contact.point.z)>0.1999f);
    }
    EXPECT_TRUE(corner);
    world.BeginSceneSync(); world.EndSceneSync();
}

TEST_F(RagdollControlsTest, HeldBodyContactsReturnOnlyAfterTheLastHandReleases)
{
    Build(2);
    physics::RigidBody other;other.SetMass(0.25f);other.SetPosition({0.1f,1,0});
    physics::OBBCollider box({0.08f,0.08f,0.08f}),floor({5,0.5f,5});
    box.Update(other.GetPosition(),math::Quaternion::Identity());floor.Update({0,-0.48f,0},math::Quaternion::Identity());
    physics::PhysicsMaterial material;physics::World world;
    world.BeginSceneSync();world.SyncBody({},&other);
    world.SyncCollider({},physics::ColliderInstance{&box,&other,&material,math::Vector3::ZERO,false,0});
    world.SyncCollider({},physics::ColliderInstance{&floor,nullptr,&material,math::Vector3::ZERO,false,0});world.EndSceneSync();
    m_rig.RefreshContacts(&world);const int original=m_rig.GetContactCount();ASSERT_GT(original,0);
    auto& c=m_rig.Controls();physics::XPBDGripSettings settings;settings.maxReach=3;settings.breakForce=0;
    ASSERT_TRUE(c.Grab("Body0",&other,other.GetPosition(),settings));
    ASSERT_TRUE(c.Grab("Body1",&other,other.GetPosition(),settings));
    m_rig.RefreshContacts(&world);const int held=m_rig.GetContactCount();
    EXPECT_GT(held,0);EXPECT_LT(held,original);
    other.m_isStatic=true;m_rig.RefreshContacts(&world);EXPECT_EQ(m_rig.GetContactCount(),original);
    other.m_isStatic=false;m_rig.RefreshContacts(&world);EXPECT_EQ(m_rig.GetContactCount(),held);
    EXPECT_FALSE(c.Grab("Body0",&other,{100,1,0},settings));
    ASSERT_TRUE(c.Release("Body0"));m_rig.RefreshContacts(&world);EXPECT_EQ(m_rig.GetContactCount(),held);
    ASSERT_TRUE(c.Grab("Body1",nullptr,c.BodyState("Body1").position,settings));
    m_rig.RefreshContacts(&world);EXPECT_EQ(m_rig.GetContactCount(),held);
    for(const auto position:{math::Vector3{10,1,0},math::Vector3{0.1f,1,0}}) {
        other.SetPosition(position);box.Update(position,other.GetRotation());
        world.BeginSceneSync();world.SyncBody({},&other);
        world.SyncCollider({},physics::ColliderInstance{&box,&other,&material,math::Vector3::ZERO,false,0});
        world.SyncCollider({},physics::ColliderInstance{&floor,nullptr,&material,math::Vector3::ZERO,false,0});world.EndSceneSync();
        m_rig.RefreshContacts(&world);
    }
    EXPECT_EQ(m_rig.GetContactCount(),original);
    c.ReleaseAll();world.BeginSceneSync();world.EndSceneSync();
}
TEST_F(RagdollControlsTest, MovingHeldColliderDoesNotAccumulateOwnerContactReactions)
{
    Build();m_rig.SetDrive(true,1,1,1);m_rig.SetSubsteps(12);
    physics::RigidBody other;other.SetMass(0.25f);other.m_useGravity=false;other.SetPosition({0.1f,1,0});
    physics::OBBCollider box({0.08f,0.08f,0.08f});other.SetInertiaFromCollider(&box);
    physics::PhysicsMaterial material;physics::World world;
    physics::BodyHandle bodyHandle;physics::ColliderHandle colliderHandle;
    auto& c=m_rig.Controls();physics::XPBDGripSettings settings;settings.breakForce=0;settings.breakDistance=0;
    ASSERT_TRUE(c.Grab("Body0",&other,other.GetPosition(),settings));
    for(int i=0;i<600;++i) {
        other.Integrate(1.0f/60);
        box.Update(other.GetPosition(),other.GetRotation());
        world.BeginSceneSync();bodyHandle=world.SyncBody(bodyHandle,&other);
        colliderHandle=world.SyncCollider(colliderHandle,physics::ColliderInstance{&box,&other,&material,math::Vector3::ZERO,false,0});world.EndSceneSync();
        scene::RagdollBodyTarget target;target.position={std::min(i/120.0f,1.0f)*0.2f,1,0};
        ASSERT_TRUE(c.SetTarget("Body0",target));m_rig.UpdateDriveTargets(m_bones,1.0f/60);
        m_rig.RefreshContacts(&world);m_rig.Step(1.0f/60);m_rig.ApplyContactReactions();
        ASSERT_TRUE(other.IsFiniteState());EXPECT_LT(other.GetVelocity().Length(),2.0f);
        EXPECT_LT((c.GrabState("Body0").anchor-c.GrabState("Body0").target).Length(),0.1f);
    }
    EXPECT_VEC3_NEAR(other.GetPosition(),math::Vector3(0.3f,1,0),0.02f);
    EXPECT_LT(other.GetVelocity().Length(),0.01f);
    c.ReleaseAll();
    for(int i=0;i<36;++i) {
        other.ApplyForce({0,-0.25f*9.81f,0});other.Integrate(1.0f/60);
        box.Update(other.GetPosition(),other.GetRotation());
        world.BeginSceneSync();bodyHandle=world.SyncBody(bodyHandle,&other);
        colliderHandle=world.SyncCollider(colliderHandle,physics::ColliderInstance{&box,&other,&material,math::Vector3::ZERO,false,0});world.EndSceneSync();
        m_rig.RefreshContacts(&world);m_rig.Step(1.0f/60);m_rig.ApplyContactReactions();
        EXPECT_LT(other.GetVelocity().Length(),8.0f);
    }
    EXPECT_LT(other.GetPosition().y,0);
    world.BeginSceneSync();world.EndSceneSync();
}

TEST_F(RagdollControlsTest, PlaneSupportAndAirborneSnapshotsHaveExplicitValidity)
{
    Build();auto a=m_rig.Controls().Balance();EXPECT_TRUE(a.valid);EXPECT_FALSE(a.hasSupport);EXPECT_FALSE(a.captureValid);
    m_rig.SetGround(0,0.9f);m_rig.SetGravity({0,-9.81f,0});
    auto b=m_rig.Controls().Balance();EXPECT_TRUE(b.hasSupport);EXPECT_TRUE(b.captureValid);EXPECT_NEAR(b.centerMargin,0.02f,1.0e-6f);
}
}
