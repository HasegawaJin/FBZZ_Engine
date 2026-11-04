/// @file    XPBDGripTests.cpp
/// @brief   有限把持の反作用・寿命・切断を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-03
#include <TestKit/TestKit.hpp>
#include <Physics/XPBDGrip.hpp>
#include <Physics/XPBDSolver.hpp>
#include <memory>
#include <limits>
namespace fbzz::tests {
class XPBDGripTest:public testkit::Fixture {};
TEST_F(XPBDGripTest, ExternalReactionConservesMomentumWithoutMovingExternalPose)
{
    physics::RigidBody a,b;a.SetMass(2);b.SetMass(3);
    physics::XPBDGrip g;physics::XPBDGripSettings s;s.breakForce=0;
    ASSERT_TRUE(g.Bind(&a,&b,math::Vector3::ZERO,s));
    a.SetVelocity({1,0,0});
    physics::XPBDSolver solver;solver.SetGravity(math::Vector3::ZERO);solver.SetSubsteps(1);solver.AddBody(&a);solver.AddTransient(&g);
    solver.Step(1.0f/120);solver.ClearTransient();solver.ClearBodies();
    EXPECT_LT(a.GetVelocity().x,1);EXPECT_GT(b.GetVelocity().x,0);
    EXPECT_NEAR(2*a.GetVelocity().x+3*b.GetVelocity().x,2,1.0e-6f);
    EXPECT_VEC3_NEAR(b.GetPosition(),math::Vector3::ZERO,1.0e-6f);
    EXPECT_LE(g.Force(),s.maxForce);
}
TEST_F(XPBDGripTest, ReachFailurePreservesOldGripAndReleaseStopsForces)
{
    physics::RigidBody a;physics::XPBDGrip g;physics::XPBDGripSettings s;
    ASSERT_TRUE(g.Bind(&a,nullptr,math::Vector3::ZERO,s));
    EXPECT_FALSE(g.Bind(&a,nullptr,{10,0,0},s));
    EXPECT_EQ(g.Status(),physics::XPBDGripStatus::HOLDING);
    s.frequency=std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(g.Bind(&a,nullptr,math::Vector3::ZERO,s));
    g.Release();a.SetVelocity({1,0,0});g.SolveVelocity(0.01f);
    EXPECT_NEAR(a.GetVelocity().x,1,1.0e-6f);
    EXPECT_EQ(g.Status(),physics::XPBDGripStatus::RELEASED);
}
TEST_F(XPBDGripTest, DistinctBreakReasonsAndExpiredBodyNeverDereferences)
{
    physics::RigidBody a;physics::XPBDGrip g;physics::XPBDGripSettings s;
    s.breakForce=1;ASSERT_TRUE(g.Bind(&a,nullptr,math::Vector3::ZERO,s));
    a.SetVelocity({10,0,0});g.ResetLambda();g.SolvePosition(0.01f);g.SolveVelocity(0.01f);
    EXPECT_EQ(g.Status(),physics::XPBDGripStatus::FORCE_LIMIT);
    a.SetVelocity(math::Vector3::ZERO);s.breakForce=0;
    ASSERT_TRUE(g.Bind(&a,nullptr,math::Vector3::ZERO,s));
    a.SetPosition({1,0,0});g.ResetLambda();g.SolvePosition(0.01f);g.SolveVelocity(0.01f);
    EXPECT_EQ(g.Status(),physics::XPBDGripStatus::DISTANCE_LIMIT);
    auto b=std::make_unique<physics::RigidBody>();a.SetPosition(math::Vector3::ZERO);
    ASSERT_TRUE(g.Bind(&a,b.get(),math::Vector3::ZERO,s));b.reset();
    EXPECT_EQ(g.Status(),physics::XPBDGripStatus::TARGET_LOST);
    g.SolveVelocity(0.01f);
    EXPECT_VEC3_NEAR(g.Target(),math::Vector3::ZERO,1.0e-6f);
}
TEST_F(XPBDGripTest, RotationLockIsOptionalAndTorqueLimited)
{
    float freeSpeed=0.0f;
    for(bool rotation:{false,true}) {
        physics::RigidBody a;a.SetMass(1);ASSERT_TRUE(a.SetInertiaDiagonal({1,1,1}));
        physics::XPBDGrip g;physics::XPBDGripSettings s;s.constrainRotation=rotation;s.maxTorque=2;s.breakForce=0;
        ASSERT_TRUE(g.Bind(&a,nullptr,math::Vector3::ZERO,s));
        a.SetAngularVelocity({0,3,0});
        physics::XPBDSolver solver;solver.SetGravity(math::Vector3::ZERO);solver.SetSubsteps(1);solver.AddBody(&a);solver.AddTransient(&g);
        solver.Step(0.01f);solver.ClearTransient();solver.ClearBodies();
        /// @note 同じquaternion積分・速度復元の丸めを除き、把持が加えた有限トルクの差だけを照合する。
        if(rotation)EXPECT_NEAR(freeSpeed-a.GetAngularVelocity().y,0.02f,1.0e-5f);
        else {freeSpeed=a.GetAngularVelocity().y;EXPECT_NEAR(freeSpeed,3,1.0e-3f);}
    }
}
TEST_F(XPBDGripTest, PushPullRemainsBoundedAcrossMassRatiosAndSubsteps)
{
    for (float mass:{0.001f,0.01f,0.1f,1.0f,10.0f,100.0f,1000.0f}) for (int steps:{1,12,24}) {
        physics::RigidBody body,other;
        body.SetMass(1); other.SetMass(mass); other.m_useGravity=false; other.m_allowSleeping=false;
        physics::XPBDGrip grip; physics::XPBDGripSettings settings;
        settings.breakForce=0; settings.breakDistance=0; settings.maxForce=100;
        ASSERT_TRUE(grip.Bind(&body,&other,math::Vector3::ZERO,settings));
        other.SetPosition({0.05f,0,0});
        physics::XPBDSolver solver; solver.SetGravity(math::Vector3::ZERO);
        solver.SetSubsteps(steps); solver.AddBody(&body); solver.AddTransient(&grip);
        for (int frame=0;frame<600;++frame) {
            other.Integrate(1.0f/120);
            solver.Step(1.0f/120);
            EXPECT_LE(grip.Force(),100.001f);
            EXPECT_LT(body.GetVelocity().Length(),5);
            EXPECT_LT(other.GetVelocity().Length(),5);
            EXPECT_NEAR(body.GetVelocity().x+mass*other.GetVelocity().x,0,0.01f);
        }
        EXPECT_LT((body.GetPosition()-other.GetPosition()).Length(),0.001f);
        solver.ClearTransient(); solver.ClearBodies();
    }
}
TEST_F(XPBDGripTest, WorldGripSettlesUnderGravityAcrossSubsteps)
{
    for(int steps:{1,4,12}) {
        physics::RigidBody a;physics::XPBDSolver solver;solver.AddBody(&a);solver.SetSubsteps(steps);
        physics::XPBDGrip g;physics::XPBDGripSettings s;s.breakForce=0;
        ASSERT_TRUE(g.Bind(&a,nullptr,math::Vector3::ZERO,s));solver.AddTransient(&g);
        for(int i=0;i<600;++i)solver.Step(1.0f/120);
        EXPECT_LT(a.GetPosition().Length(),0.03f);EXPECT_LT(a.GetVelocity().Length(),0.01f);
        solver.ClearTransient();solver.ClearBodies();
    }
}
}
