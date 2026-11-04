/// @file    XPBDJointVelocityTests.cpp
/// @brief   脱力中の接続点速度と自由回転・角運動量を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-03
#include <TestKit/TestKit.hpp>
#include <Physics/XPBDJoint.hpp>
#include <Physics/XPBDSolver.hpp>
#include <cmath>
namespace fbzz::tests {
class XPBDJointVelocityTest : public testkit::Fixture {};
namespace {
class GeometricRotation final : public physics::XPBDConstraint {
public:
    explicit GeometricRotation(physics::RigidBody& body) : m_body(body) {}
    void ResetLambda() override {}
    void SolvePosition(float) override { m_body.SetRotation(math::Quaternion::FromAxisAngle(math::Vector3::FORWARD,1.57079632679f)); }
    void SolvePhysicalPosition(float) override {}
    void SolveCorrectionPosition(float h) override { SolvePosition(h); }
private:
    physics::RigidBody& m_body;
};
}
TEST_F(XPBDJointVelocityTest, GeometricRotationDoesNotPumpAnisotropicKineticEnergy)
{
    physics::RigidBody body; body.SetMass(1);
    ASSERT_TRUE(body.SetInertiaDiagonal({0.01f,1.0f,0.2f}));
    body.m_linearDrag=0; body.m_angularDrag=0; body.SetAngularVelocity({2,0,0});
    physics::XPBDSolver solver; solver.SetGravity(math::Vector3::ZERO); solver.SetSubsteps(1);
    solver.AddBody(&body); solver.AddConstraint(std::make_unique<GeometricRotation>(body));
    solver.Step(1.0f/60);
    const auto local=body.GetRotation().Inverse()*body.GetAngularVelocity();
    const float energy=0.5f*(0.01f*local.x*local.x+local.y*local.y+0.2f*local.z*local.z);
    EXPECT_VEC3_NEAR(body.GetRotation()*math::Vector3::UP,-math::Vector3::RIGHT,testkit::kTolerance);
    EXPECT_GT(body.GetAngularVelocity().Length(),0.1f);
    EXPECT_LE(energy,0.020001f);
    solver.ClearConstraints(); solver.ClearBodies();
}
TEST_F(XPBDJointVelocityTest, FreeAnisotropicRotationRetainsItsPhysicalEnergy)
{
    for (int steps:{1,12,24,48}) {
        physics::RigidBody body; body.SetMass(1);
        ASSERT_TRUE(body.SetInertiaDiagonal({0.01f,1.0f,0.2f}));
        body.m_linearDrag=0; body.m_angularDrag=0; body.SetAngularVelocity({1,2,3});
        physics::XPBDSolver solver; solver.SetGravity(math::Vector3::ZERO); solver.SetSubsteps(steps);
        solver.AddBody(&body); solver.Step(1.0f/60);
        const auto local=body.GetRotation().Inverse()*body.GetAngularVelocity();
        const float energy=0.5f*(0.01f*local.x*local.x+local.y*local.y+0.2f*local.z*local.z);
        EXPECT_GT(energy,2.90f);
        EXPECT_LT(energy,2.906f);
        EXPECT_GT(body.GetAngularVelocity().Length(),3.73f);
        solver.ClearBodies();
    }
}
TEST_F(XPBDJointVelocityTest, PassiveSocketPreservesFreeRotationAndAngularMomentum)
{
    physics::RigidBody parent,child;
    parent.SetMass(1); parent.m_isStatic=true;
    child.SetMass(1); ASSERT_TRUE(child.SetInertiaDiagonal({1,1,1}));
    child.SetPosition({1,0,0});
    physics::XPBDJoint joint(&parent,&child);
    joint.Build(math::Vector3::ZERO,math::Quaternion::Identity());
    joint.Drive().enabled=false;
    child.SetVelocity({0,1,0});
    child.SetAngularVelocity(math::Vector3::ZERO);
    joint.SolveVelocity(1.0f/60);
    const auto velocity=child.GetVelocity();
    const auto angular=child.GetAngularVelocity();
    const auto anchorVelocity=velocity+math::Vector3::Cross(angular,-child.GetPosition());
    EXPECT_VEC3_NEAR(anchorVelocity,math::Vector3::ZERO,testkit::kLooseTolerance);
    EXPECT_GT(angular.z,0.1f);
    EXPECT_NEAR(velocity.y+angular.z,1,testkit::kLooseTolerance);
}
TEST_F(XPBDJointVelocityTest, LimitStopsOnlyOutwardAngularVelocity)
{
    physics::RigidBody parent,child;
    parent.SetMass(1); parent.m_isStatic=true;
    child.SetMass(1); ASSERT_TRUE(child.SetInertiaDiagonal({1,1,1}));
    child.SetPosition({1,0,0});
    physics::XPBDJoint joint(&parent,&child);
    joint.Build(math::Vector3::ZERO,math::Quaternion::Identity());
    joint.Limits().enabled=true; joint.Limits().twistMax=0.2f; joint.Limits().twistMin=-0.2f;
    child.SetRotation(math::Quaternion::FromAxisAngle(math::Vector3::RIGHT,0.2f));
    child.SetAngularVelocity({2,0,0}); joint.SolveVelocity(1.0f/60);
    EXPECT_NEAR(child.GetAngularVelocity().x,0,testkit::kTolerance);
    child.SetAngularVelocity({-2,0,0}); joint.SolveVelocity(1.0f/60);
    EXPECT_NEAR(child.GetAngularVelocity().x,-2,testkit::kTolerance);
}
TEST_F(XPBDJointVelocityTest, SoftConstraintsRetainTheirSpringMotion)
{
    physics::RigidBody parent,child;
    parent.SetMass(1); parent.m_isStatic=true;
    child.SetMass(1); child.SetPosition({1,0,0});
    physics::XPBDJoint joint(&parent,&child);
    joint.Build(math::Vector3::ZERO,math::Quaternion::Identity());
    joint.SetSocketCompliance(0.01f);
    joint.Limits().enabled=true; joint.Limits().compliance=0.01f; joint.Limits().twistMax=0.2f;
    child.SetRotation(math::Quaternion::FromAxisAngle(math::Vector3::RIGHT,0.2f));
    child.SetVelocity({0,1,0}); child.SetAngularVelocity({2,0,0});
    joint.SolveVelocity(1.0f/60);
    EXPECT_NEAR(child.GetVelocity().y,1,testkit::kTolerance);
    EXPECT_NEAR(child.GetAngularVelocity().x,2,testkit::kTolerance);
}
TEST_F(XPBDJointVelocityTest, SocketProjectionDoesNotRestartOutwardMotionAtASwingLimit)
{
    physics::RigidBody parent,child;
    parent.SetMass(1); parent.m_isStatic=true;
    child.SetMass(1); ASSERT_TRUE(child.SetInertiaDiagonal({0.01f,0.01f,0.01f}));
    child.SetPosition({1,0,0});
    physics::XPBDJoint joint(&parent,&child);
    joint.Build(math::Vector3::ZERO,math::Quaternion::Identity());
    joint.Limits().enabled=true;
    joint.Limits().swingMinY=-0.2f; joint.Limits().swingMaxY=0.2f;
    child.SetRotation(math::Quaternion::FromAxisAngle(math::Vector3::UP,0.2f));
    child.SetVelocity({0,0,10});
    joint.SolveVelocity(1.0f/60);
    const auto arm=child.GetRotation()*math::Vector3{-1,0,0};
    const auto anchorVelocity=child.GetVelocity()+math::Vector3::Cross(child.GetAngularVelocity(),arm);
    EXPECT_LT(child.GetAngularVelocity().y,0.001f);
    EXPECT_LT(anchorVelocity.Length(),0.001f);
}
TEST_F(XPBDJointVelocityTest, HardJointProjectionDoesNotCreateFreeSwingEnergy)
{
    for (int steps:{1,12,24,48}) {
        physics::RigidBody parent,child;
        parent.SetMass(1); parent.m_isStatic=true; child.SetMass(1);
        ASSERT_TRUE(child.SetInertiaDiagonal({1,1,1})); child.SetPosition({1,0,0});
        auto joint=std::make_unique<physics::XPBDJoint>(&parent,&child);
        joint->Build(math::Vector3::ZERO,math::Quaternion::Identity());
        child.SetPosition({1,0.05f,0});
        physics::XPBDSolver solver; solver.SetGravity(math::Vector3::ZERO); solver.SetSubsteps(steps);
        solver.AddBody(&child); solver.AddConstraint(std::move(joint)); solver.Step(1.0f/60);
        EXPECT_LT(child.GetVelocity().Length(),0.001f);
        EXPECT_LT(child.GetAngularVelocity().Length(),0.001f);
        solver.ClearConstraints(); solver.ClearBodies();
    }
}
TEST_F(XPBDJointVelocityTest, LargeRotationalCorrectionsStayInsideTheLinearizationRegion)
{
    const auto turn=physics::IntegrateRotation(math::Quaternion::Identity(),{0,10000,0},0.001f);
    EXPECT_GT(turn.w,0.96f);
    EXPECT_NEAR(turn.Length(),1,testkit::kTolerance);
    const auto small=physics::IntegrateRotation(math::Quaternion::Identity(),{0,1,0},0.01f);
    EXPECT_NEAR(small.y,0.005f,1.0e-6f);
}
TEST_F(XPBDJointVelocityTest, BentJointPreservesTwistInsideTheSwingLimits)
{
    for (float angle:{0.0f,0.00001f,1.57079632679f}) {
        physics::RigidBody parent,child;
        parent.SetMass(1); parent.m_isStatic=true; child.SetMass(1);
        ASSERT_TRUE(child.SetInertiaDiagonal({1,1,1}));
        const auto frame=math::Quaternion::FromAxisAngle(math::Vector3{1,2,3}.Normalized(),0.7f);
        parent.SetRotation(frame); child.SetRotation(frame);
        physics::XPBDJoint joint(&parent,&child);
        joint.Build(math::Vector3::ZERO,frame);
        joint.Limits().enabled=true;
        joint.Limits().swingMinZ=-2; joint.Limits().swingMaxZ=2;
        joint.Limits().twistMin=-3; joint.Limits().twistMax=3;
        child.SetRotation(frame*math::Quaternion::FromAxisAngle(math::Vector3::FORWARD,angle));
        const auto velocity=child.GetRotation()*math::Vector3{2,0,0};
        child.SetAngularVelocity(velocity);
        joint.SolveVelocity(1.0f/60);
        EXPECT_VEC3_NEAR(child.GetAngularVelocity(),velocity,testkit::kTolerance);
    }
}
TEST_F(XPBDJointVelocityTest, MixedSwingLimitStopsOnlyTheOutwardAngleRateAndPreservesMomentum)
{
    physics::RigidBody parent,child;
    parent.SetMass(1); child.SetMass(1);
    ASSERT_TRUE(parent.SetInertiaDiagonal({1,1,1}));
    ASSERT_TRUE(child.SetInertiaDiagonal({1,1,1}));
    physics::XPBDJoint joint(&parent,&child);
    joint.Build(math::Vector3::ZERO,math::Quaternion::Identity());
    joint.Limits().enabled=true;
    joint.Limits().swingMinY=-2; joint.Limits().swingMaxY=0.6f;
    joint.Limits().swingMinZ=-2; joint.Limits().swingMaxZ=2;
    joint.Limits().twistMin=-3; joint.Limits().twistMax=3;
    child.SetRotation(math::Quaternion::FromAxisAngle({0,0.6f,0.8f},1.0f));
    parent.SetAngularVelocity({1,0,0}); child.SetAngularVelocity({-1,2,-1});
    const auto momentum=parent.GetAngularVelocity()+child.GetAngularVelocity();
    joint.SolveVelocity(1.0f/60);
    EXPECT_VEC3_NEAR(parent.GetAngularVelocity()+child.GetAngularVelocity(),momentum,testkit::kTolerance);
    const auto parentNext=physics::IntegrateRotation(parent.GetRotation(),parent.GetAngularVelocity(),0.001f);
    const auto childNext=physics::IntegrateRotation(child.GetRotation(),child.GetAngularVelocity(),0.001f);
    math::Quaternion swing,twist;
    physics::DecomposeSwingTwist((parentNext.Inverse()*childNext).Normalized(),swing,twist);
    EXPECT_LE(physics::RotationVector(swing).y,0.60001f);
    parent.SetAngularVelocity(math::Vector3::ZERO); child.SetAngularVelocity({2,0,0});
    joint.SolveVelocity(1.0f/60);
    EXPECT_VEC3_NEAR(child.GetAngularVelocity(),math::Vector3(2,0,0),testkit::kTolerance);
    EXPECT_VEC3_NEAR(parent.GetAngularVelocity(),math::Vector3::ZERO,testkit::kTolerance);
}
TEST_F(XPBDJointVelocityTest, BentTwistLimitUsesTheDerivativeOfTheTwistAngle)
{
    physics::RigidBody parent,child;
    parent.SetMass(1); parent.m_isStatic=true; child.SetMass(1);
    ASSERT_TRUE(child.SetInertiaDiagonal({1,1,1}));
    physics::XPBDJoint joint(&parent,&child);
    joint.Build(math::Vector3::ZERO,math::Quaternion::Identity());
    joint.Limits().enabled=true;
    joint.Limits().swingMinY=-3; joint.Limits().swingMaxY=3;
    joint.Limits().swingMinZ=-3; joint.Limits().swingMaxZ=3;
    joint.Limits().twistMin=-0.5f; joint.Limits().twistMax=0.2f;
    child.SetRotation(math::Quaternion::FromAxisAngle(math::Vector3::FORWARD,1.57079632679f)
        *math::Quaternion::FromAxisAngle(math::Vector3::RIGHT,0.2f));
    child.SetAngularVelocity({2,0,0});
    joint.SolveVelocity(1.0f/60);
    math::Quaternion swing,twist;
    physics::DecomposeSwingTwist(physics::IntegrateRotation(child.GetRotation(),child.GetAngularVelocity(),0.001f),swing,twist);
    EXPECT_LE(2.0f*std::atan2(twist.x,twist.w),0.20001f);
    EXPECT_GT(child.GetAngularVelocity().Length(),0.5f);
}
TEST_F(XPBDJointVelocityTest, AntipodalSwingDoesNotGenerateNonfiniteVelocity)
{
    for (float angle:{3.1415f,3.14159265359f}) {
        physics::RigidBody parent,child;
        parent.SetMass(1); parent.m_isStatic=true; child.SetMass(1);
        ASSERT_TRUE(child.SetInertiaDiagonal({1,1,1}));
        physics::XPBDJoint joint(&parent,&child);
        joint.Build(math::Vector3::ZERO,math::Quaternion::Identity());
        joint.Limits().enabled=true;
        child.SetRotation(math::Quaternion::FromAxisAngle(math::Vector3::FORWARD,angle));
        child.SetAngularVelocity({1,2,3});
        joint.SolveVelocity(1.0f/60);
        EXPECT_TRUE(child.IsFiniteState());
        EXPECT_LE(child.GetAngularVelocity().LengthSq(),14.0001f);
    }
}
TEST_F(XPBDJointVelocityTest, LegacyDriveSharesTheTorqueBudgetWithDampingAndReturnsReaction)
{
    constexpr float h=0.001f;
    for (const auto initial:{math::Vector3{1,0,0},math::Vector3{-1,0,0},math::Vector3{0,1,0}}) {
        physics::RigidBody parent,child;
        parent.SetMass(1); child.SetMass(1);
        ASSERT_TRUE(parent.SetInertiaDiagonal({1,1,1}));
        ASSERT_TRUE(child.SetInertiaDiagonal({1,1,1}));
        parent.m_linearDrag=0; parent.m_angularDrag=0;
        child.m_linearDrag=0; child.m_angularDrag=0;
        auto joint=std::make_unique<physics::XPBDJoint>(&parent,&child);
        joint->Build(math::Vector3::ZERO,math::Quaternion::Identity());
        joint->Drive().enabled=true; joint->Drive().compliance=0.00001f;
        joint->Drive().damping=1000; joint->Drive().maxTorque=2;
        joint->Drive().target=math::Quaternion::FromAxisAngle(math::Vector3::RIGHT,-1.0f);
        child.SetAngularVelocity(initial);
        physics::XPBDSolver solver; solver.SetGravity(math::Vector3::ZERO); solver.SetSubsteps(1);
        solver.AddBody(&parent); solver.AddBody(&child); solver.AddConstraint(std::move(joint));
        solver.Step(h);
        const auto impulse=child.GetAngularVelocity()-initial;
        EXPECT_LE(impulse.Length(),2.0f*h+0.00001f);
        EXPECT_VEC3_NEAR(parent.GetAngularVelocity()+child.GetAngularVelocity(),initial,0.00001f);
        if (initial.x<0.0f) EXPECT_GT(impulse.x,0.0019f);
        solver.ClearConstraints(); solver.ClearBodies();
    }
}
} /// @note namespace fbzz::tests
