/// @file    XPBDBodyDriveTests.cpp
/// @brief   有限駆動の追従・反作用・出力制限を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-03
#include <TestKit/TestKit.hpp>
#include <Physics/XPBDBodyDrive.hpp>
#include <Physics/XPBDSolver.hpp>
#include <Math/MathUtils.hpp>
#include <cmath>
#include <memory>

namespace fbzz::tests {
class XPBDBodyDriveTest : public testkit::Fixture {};

TEST_F(XPBDBodyDriveTest, HoldsGravityAtSameDeflectionAcrossMassAndSubsteps)
{
    for (int substeps : {1,4,12}) for (float mass : {1.0f,20.0f}) {
        physics::RigidBody body;
        body.SetMass(mass);
        physics::XPBDSolver solver;
        solver.SetSubsteps(substeps);
        solver.AddBody(&body);
        auto drive=std::make_unique<physics::XPBDBodyDrive>(&body);
        physics::XPBDBodyDriveSettings settings;
        settings.enabled=true; settings.linearFrequency=4; settings.maxForce=10000;
        drive->SetSettings(settings);
        drive->SetTarget({0,1,0},math::Quaternion::Identity());
        solver.AddConstraint(std::move(drive));
        for (int i=0;i<480;++i) solver.Step(1.0f/120.0f);
        const float omega=math::TWO_PI*4;
        EXPECT_NEAR(body.GetPosition().y,1.0f-9.81f/(omega*omega),0.002f);
        EXPECT_LT(body.GetVelocity().Length(),0.002f);
        solver.ClearBodies();
    }
}

TEST_F(XPBDBodyDriveTest, ForceAndTorqueIncludeDampingWithinSingleBudget)
{
    physics::RigidBody body;
    body.SetMass(2);
    ASSERT_TRUE(body.SetInertiaDiagonal({2,2,2}));
    body.SetVelocity({5,0,0}); body.SetAngularVelocity({0,4,0});
    physics::XPBDBodyDrive drive(&body);
    physics::XPBDBodyDriveSettings settings;
    settings.enabled=true; settings.linearFrequency=10; settings.angularFrequency=10;
    settings.maxForce=3; settings.maxTorque=4;
    drive.SetSettings(settings);
    drive.SetTarget({-100,0,0},math::Quaternion::FromAxisAngle(math::Vector3::UP,-2));
    const float h=1.0f/120;
    drive.SolvePosition(h);
    EXPECT_LE(body.GetPosition().Length(),3.0f/2*h*h+1.0e-6f);
    EXPECT_LE(physics::RotationVector(body.GetRotation()).Length(),4.0f/2*h*h+1.0e-6f);
}

TEST_F(XPBDBodyDriveTest, ParentDriveConservesMomentumAndLeavesUncontrolledAxesFree)
{
    physics::RigidBody child,parent;
    child.SetMass(2); parent.SetMass(3);
    child.SetPosition({2,0,0});
    physics::XPBDBodyDrive drive(&child);
    drive.SetReference(&parent);
    physics::XPBDBodyDriveSettings settings;
    settings.enabled=true; settings.linearFrequency=5; settings.maxForce=1000; settings.maxTorque=0;
    drive.SetSettings(settings);
    drive.SetTarget({1,0,0},math::Quaternion::FromAxisAngle(math::Vector3::UP,1));
    drive.SolvePosition(0.01f);
    EXPECT_LT(child.GetPosition().x,2);
    EXPECT_GT(parent.GetPosition().x,0);
    EXPECT_NEAR(2*child.GetPosition().x+3*parent.GetPosition().x,4,1.0e-6f);
    EXPECT_VEC3_NEAR(physics::RotationVector(child.GetRotation()),math::Vector3::ZERO,1.0e-6f);
    settings.maxForce=0;
    drive.SetSettings(settings); drive.ResetLambda();
    const auto p=child.GetPosition();
    drive.SolvePosition(0.01f);
    EXPECT_VEC3_NEAR(child.GetPosition(),p,1.0e-6f);
}

TEST_F(XPBDBodyDriveTest, MatchingTargetVelocityAvoidsDragOnAnimation)
{
    physics::RigidBody body;
    body.SetVelocity({1,0,0}); body.SetAngularVelocity({0,1,0});
    physics::XPBDBodyDrive drive(&body);
    physics::XPBDBodyDriveSettings settings;
    settings.enabled=true; settings.linearFrequency=5; settings.maxForce=1000;
    drive.SetSettings(settings);
    drive.SetTarget(math::Vector3::ZERO,math::Quaternion::Identity(),{1,0,0},{0,1,0});
    drive.SolvePosition(0.01f);
    EXPECT_VEC3_NEAR(body.GetPosition(),math::Vector3::ZERO,1.0e-6f);
    EXPECT_VEC3_NEAR(physics::RotationVector(body.GetRotation()),math::Vector3::ZERO,1.0e-6f);
    settings.targetVelocityMultiplier=0;
    drive.SetSettings(settings);
    drive.SolvePosition(0.01f);
    EXPECT_LT(body.GetPosition().x,0);
    EXPECT_LT(physics::RotationVector(body.GetRotation()).y,0);
}
}
