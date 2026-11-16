/// @file    ConstraintTests.cpp
/// @brief   剛体制約 (Distance / Rope / Spring / Chain / Slider / Fixed / Hinge) の補正方向と質量配分を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// 位置制約は «どちらをどれだけ動かすか» が逆質量の比で決まる。ここが崩れても
/// 見た目は «少し引っ張られている» だけで、静止した親が子に引きずられる、という
/// 一番追いにくい形で表面化する。
#include <TestKit/TestKit.hpp>

#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Physics/ChainConstraint.hpp>
#include <Physics/DistanceConstraint.hpp>
#include <Physics/FixedConstraint.hpp>
#include <Physics/HingeConstraint.hpp>
#include <Physics/RopeConstraint.hpp>
#include <Physics/SliderConstraint.hpp>
#include <Physics/SpringConstraint.hpp>

#include <cmath>
#include <vector>

namespace fbzz::tests {
namespace {

physics::RigidBody MakeStaticAt(const math::Vector3& position)
{
    physics::RigidBody body;
    body.m_isStatic = true;
    body.SetMass(1.0f);
    body.SetPosition(position);
    return body;
}

physics::RigidBody MakeDynamicAt(const math::Vector3& position, float mass = 1.0f)
{
    physics::RigidBody body;
    body.SetMass(mass);
    body.SetPosition(position);
    return body;
}

} // namespace

class ConstraintTest : public testkit::Fixture {};

/// @name 距離

/// @brief Distance pulls the dynamic body to the rest length.
TEST_F(ConstraintTest, DistancePullsTheDynamicBodyToTheRestLength)
{
    physics::RigidBody anchor  = MakeStaticAt(math::Vector3::ZERO);
    physics::RigidBody hanging = MakeDynamicAt({3.0f, 0.0f, 0.0f});
    physics::DistanceConstraint constraint(&anchor, &hanging, 1.0f);

    constraint.SolvePosition(testkit::kFixedDeltaTime);

    EXPECT_VEC3_NEAR(hanging.GetPosition(), math::Vector3(1.0f, 0.0f, 0.0f),
                     testkit::kTolerance);
    EXPECT_VEC3_NEAR(anchor.GetPosition(), math::Vector3::ZERO, testkit::kTolerance);
}

/// @brief Distance pushes apart when too close.
TEST_F(ConstraintTest, DistancePushesApartWhenTooClose)
{
    physics::RigidBody anchor  = MakeStaticAt(math::Vector3::ZERO);
    physics::RigidBody hanging = MakeDynamicAt({0.25f, 0.0f, 0.0f});
    physics::DistanceConstraint constraint(&anchor, &hanging, 1.0f);

    constraint.SolvePosition(testkit::kFixedDeltaTime);

    EXPECT_VEC3_NEAR(hanging.GetPosition(), math::Vector3(1.0f, 0.0f, 0.0f),
                     testkit::kTolerance);
}

/// @brief Distance splits the correction by inverse mass.
TEST_F(ConstraintTest, DistanceSplitsTheCorrectionByInverseMass)
{
    /// @note 重い側ほど動かない。等質量なら半分ずつ。
    physics::RigidBody light = MakeDynamicAt(math::Vector3::ZERO, 1.0f);
    physics::RigidBody heavy = MakeDynamicAt({4.0f, 0.0f, 0.0f}, 3.0f);
    physics::DistanceConstraint constraint(&light, &heavy, 2.0f);

    constraint.SolvePosition(testkit::kFixedDeltaTime);

    EXPECT_NEAR((heavy.GetPosition() - light.GetPosition()).Length(), 2.0f, testkit::kTolerance);
    EXPECT_NEAR(light.GetPosition().x, 1.5f, testkit::kTolerance);
    EXPECT_NEAR(heavy.GetPosition().x, 3.5f, testkit::kTolerance);
}

/// @brief Distance does nothing when both bodies are static.
TEST_F(ConstraintTest, DistanceDoesNothingWhenBothBodiesAreStatic)
{
    physics::RigidBody a = MakeStaticAt(math::Vector3::ZERO);
    physics::RigidBody b = MakeStaticAt({9.0f, 0.0f, 0.0f});
    physics::DistanceConstraint constraint(&a, &b, 1.0f);

    constraint.SolvePosition(testkit::kFixedDeltaTime);

    EXPECT_VEC3_NEAR(b.GetPosition(), math::Vector3(9.0f, 0.0f, 0.0f), testkit::kTolerance);
}

/// @brief Distance reports its type.
TEST_F(ConstraintTest, DistanceReportsItsType)
{
    physics::RigidBody a;
    physics::RigidBody b;
    physics::DistanceConstraint constraint(&a, &b, 1.0f);

    EXPECT_EQ(constraint.GetType(), physics::ConstraintType::DISTANCE);
    EXPECT_EQ(constraint.GetBodyA(), &a);
    EXPECT_EQ(constraint.GetBodyB(), &b);
}

/// @name ロープ

/// @brief Rope leaves slack alone.
TEST_F(ConstraintTest, RopeLeavesSlackAlone)
{
    /// @note たるみを表現できることがロープの存在理由。縮めたら «棒» になる。
    physics::RigidBody anchor  = MakeStaticAt(math::Vector3::ZERO);
    physics::RigidBody hanging = MakeDynamicAt({0.5f, 0.0f, 0.0f});
    physics::RopeConstraint constraint(&anchor, &hanging, 2.0f);

    constraint.SolvePosition(testkit::kFixedDeltaTime);

    EXPECT_VEC3_NEAR(hanging.GetPosition(), math::Vector3(0.5f, 0.0f, 0.0f),
                     testkit::kTolerance);
}

/// @brief Rope clamps to the maximum length.
TEST_F(ConstraintTest, RopeClampsToTheMaximumLength)
{
    physics::RigidBody anchor  = MakeStaticAt(math::Vector3::ZERO);
    physics::RigidBody hanging = MakeDynamicAt({0.0f, -8.0f, 0.0f});
    physics::RopeConstraint constraint(&anchor, &hanging, 2.0f);

    constraint.SolvePosition(testkit::kFixedDeltaTime);

    EXPECT_VEC3_NEAR(hanging.GetPosition(), math::Vector3(0.0f, -2.0f, 0.0f),
                     testkit::kTolerance);
}

/// @name バネ

/// @brief Spring is at rest at the rest length.
TEST_F(ConstraintTest, SpringIsAtRestAtTheRestLength)
{
    physics::RigidBody a = MakeDynamicAt(math::Vector3::ZERO);
    physics::RigidBody b = MakeDynamicAt({1.0f, 0.0f, 0.0f});
    physics::SpringConstraint constraint(&a, &b, 1.0f, 10.0f, 0.0f);

    constraint.ApplyForce(testkit::kFixedDeltaTime);
    a.Integrate(testkit::kFixedDeltaTime);
    b.Integrate(testkit::kFixedDeltaTime);

    EXPECT_VEC3_NEAR(a.GetVelocity(), math::Vector3::ZERO, testkit::kTolerance);
    EXPECT_VEC3_NEAR(b.GetVelocity(), math::Vector3::ZERO, testkit::kTolerance);
}

/// @brief Spring pulls the bodies together when stretched.
TEST_F(ConstraintTest, SpringPullsTheBodiesTogetherWhenStretched)
{
    physics::RigidBody a = MakeDynamicAt(math::Vector3::ZERO);
    physics::RigidBody b = MakeDynamicAt({3.0f, 0.0f, 0.0f});
    physics::SpringConstraint constraint(&a, &b, 1.0f, 10.0f, 0.0f);

    constraint.ApplyForce(testkit::kFixedDeltaTime);
    a.Integrate(testkit::kFixedDeltaTime);
    b.Integrate(testkit::kFixedDeltaTime);

    EXPECT_GT(a.GetVelocity().x, 0.0f);
    EXPECT_LT(b.GetVelocity().x, 0.0f);
}

/// @brief Spring pushes the bodies apart when compressed.
TEST_F(ConstraintTest, SpringPushesTheBodiesApartWhenCompressed)
{
    physics::RigidBody a = MakeDynamicAt(math::Vector3::ZERO);
    physics::RigidBody b = MakeDynamicAt({0.5f, 0.0f, 0.0f});
    physics::SpringConstraint constraint(&a, &b, 1.0f, 10.0f, 0.0f);

    constraint.ApplyForce(testkit::kFixedDeltaTime);
    a.Integrate(testkit::kFixedDeltaTime);
    b.Integrate(testkit::kFixedDeltaTime);

    EXPECT_LT(a.GetVelocity().x, 0.0f);
    EXPECT_GT(b.GetVelocity().x, 0.0f);
}

/// @brief Checks spring damping opposes the separation speed.
TEST_F(ConstraintTest, SpringDampingOpposesTheSeparationSpeed)
{
    /// @note 伸びは 0 なのでバネ力は 0。残るのは減衰だけで、離れる動きを止める向きに働く。
    physics::RigidBody a = MakeDynamicAt(math::Vector3::ZERO);
    physics::RigidBody b = MakeDynamicAt({1.0f, 0.0f, 0.0f});
    b.SetVelocity({2.0f, 0.0f, 0.0f});
    physics::SpringConstraint constraint(&a, &b, 1.0f, 10.0f, 5.0f);

    constraint.ApplyForce(testkit::kFixedDeltaTime);
    b.Integrate(testkit::kFixedDeltaTime);

    EXPECT_LT(b.GetVelocity().x, 2.0f);
}

/// @name 鎖

/// @brief Checks chain converges to the segment length between neighbours.
TEST_F(ConstraintTest, ChainConvergesToTheSegmentLengthBetweenNeighbours)
{
    physics::RigidBody a = MakeDynamicAt(math::Vector3::ZERO);
    physics::RigidBody b = MakeDynamicAt({5.0f, 0.0f, 0.0f});
    physics::RigidBody c = MakeDynamicAt({10.0f, 0.0f, 0.0f});
    physics::ChainConstraint chain(std::vector<physics::RigidBody*>{&a, &b, &c}, 1.0f, 64);

    chain.SolvePosition(testkit::kFixedDeltaTime);

    EXPECT_NEAR((b.GetPosition() - a.GetPosition()).Length(), 1.0f, testkit::kLooseTolerance);
    EXPECT_NEAR((c.GetPosition() - b.GetPosition()).Length(), 1.0f, testkit::kLooseTolerance);
}

/// @brief Chain keeps the centre of mass when every body is dynamic.
TEST_F(ConstraintTest, ChainKeepsTheCentreOfMassWhenEveryBodyIsDynamic)
{
    /// @note 補正は逆質量の比で分けるので、外力なしに全体が横滑りしてはいけない。
    physics::RigidBody a = MakeDynamicAt(math::Vector3::ZERO);
    physics::RigidBody b = MakeDynamicAt({5.0f, 0.0f, 0.0f});
    physics::ChainConstraint chain(std::vector<physics::RigidBody*>{&a, &b}, 1.0f, 8);

    chain.SolvePosition(testkit::kFixedDeltaTime);

    EXPECT_NEAR((a.GetPosition().x + b.GetPosition().x) * 0.5f, 2.5f, testkit::kLooseTolerance);
}

/// @brief Chain ignores null and single body configurations.
TEST_F(ConstraintTest, ChainIgnoresNullAndSingleBodyConfigurations)
{
    physics::RigidBody only = MakeDynamicAt({5.0f, 0.0f, 0.0f});
    physics::ChainConstraint single(std::vector<physics::RigidBody*>{&only}, 1.0f, 4);
    physics::ChainConstraint withHole(std::vector<physics::RigidBody*>{&only, nullptr}, 1.0f, 4);

    single.SolvePosition(testkit::kFixedDeltaTime);
    withHole.SolvePosition(testkit::kFixedDeltaTime);

    EXPECT_VEC3_NEAR(only.GetPosition(), math::Vector3(5.0f, 0.0f, 0.0f), testkit::kTolerance);
}

/// @name スライダー

/// @brief Slider removes the off axis offset and keeps the axial one.
TEST_F(ConstraintTest, SliderRemovesTheOffAxisOffsetAndKeepsTheAxialOne)
{
    physics::RigidBody rail    = MakeStaticAt(math::Vector3::ZERO);
    physics::RigidBody carriage = MakeDynamicAt({2.0f, 1.0f, -1.0f});
    physics::SliderConstraint constraint(&rail, &carriage, math::Vector3::RIGHT);

    constraint.SolvePosition(testkit::kFixedDeltaTime);

    EXPECT_VEC3_NEAR(carriage.GetPosition(), math::Vector3(2.0f, 0.0f, 0.0f),
                     testkit::kTolerance);
}

/// @brief Slider clamps travel to the configured limits.
TEST_F(ConstraintTest, SliderClampsTravelToTheConfiguredLimits)
{
    physics::RigidBody rail     = MakeStaticAt(math::Vector3::ZERO);
    physics::RigidBody carriage = MakeDynamicAt({5.0f, 0.0f, 0.0f});
    physics::SliderConstraint constraint(&rail, &carriage, math::Vector3::RIGHT);
    constraint.SetLimits(0.0f, 1.0f);

    constraint.SolvePosition(testkit::kFixedDeltaTime);

    EXPECT_VEC3_NEAR(carriage.GetPosition(), math::Vector3(1.0f, 0.0f, 0.0f),
                     testkit::kTolerance);
}

/// @brief Slider limits are order independent.
TEST_F(ConstraintTest, SliderLimitsAreOrderIndependent)
{
    physics::RigidBody rail     = MakeStaticAt(math::Vector3::ZERO);
    physics::RigidBody carriage = MakeDynamicAt({5.0f, 0.0f, 0.0f});
    physics::SliderConstraint constraint(&rail, &carriage, math::Vector3::RIGHT);

    /// @note 逆順に渡しても同じ区間になる
    constraint.SetLimits(1.0f, 0.0f);

    EXPECT_NEAR(constraint.m_minDistance, 0.0f, testkit::kTolerance);
    EXPECT_NEAR(constraint.m_maxDistance, 1.0f, testkit::kTolerance);
}

/// @brief Slider travel is unbounded after clearing limits.
TEST_F(ConstraintTest, SliderTravelIsUnboundedAfterClearingLimits)
{
    physics::RigidBody rail     = MakeStaticAt(math::Vector3::ZERO);
    physics::RigidBody carriage = MakeDynamicAt({5.0f, 0.0f, 0.0f});
    physics::SliderConstraint constraint(&rail, &carriage, math::Vector3::RIGHT);
    constraint.SetLimits(0.0f, 1.0f);

    constraint.ClearLimits();
    constraint.SolvePosition(testkit::kFixedDeltaTime);

    EXPECT_VEC3_NEAR(carriage.GetPosition(), math::Vector3(5.0f, 0.0f, 0.0f),
                     testkit::kTolerance);
}

/// @name 溶接

/// @brief Fixed restores the offset captured at construction.
TEST_F(ConstraintTest, FixedRestoresTheOffsetCapturedAtConstruction)
{
    physics::RigidBody parent = MakeStaticAt(math::Vector3::ZERO);
    physics::RigidBody child  = MakeDynamicAt({2.0f, 0.0f, 0.0f});
    physics::FixedConstraint constraint(&parent, &child);

    child.SetPosition({2.0f, 5.0f, 0.0f});
    constraint.SolvePosition(testkit::kFixedDeltaTime);

    EXPECT_VEC3_NEAR(child.GetPosition(), math::Vector3(2.0f, 0.0f, 0.0f), testkit::kTolerance);
}

/// @brief Fixed carries the child around when the parent rotates.
TEST_F(ConstraintTest, FixedCarriesTheChildAroundWhenTheParentRotates)
{
    physics::RigidBody parent = MakeStaticAt(math::Vector3::ZERO);
    physics::RigidBody child  = MakeDynamicAt({2.0f, 0.0f, 0.0f});
    physics::FixedConstraint constraint(&parent, &child);

    const math::Quaternion turn =
        math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(90.0f));
    parent.SetRotation(turn);
    constraint.SolvePosition(testkit::kFixedDeltaTime);

    EXPECT_VEC3_NEAR(child.GetPosition(), turn * math::Vector3(2.0f, 0.0f, 0.0f),
                     testkit::kLooseTolerance);
    EXPECT_QUAT_NEAR(child.GetRotation(), turn, testkit::kLooseTolerance);
}

/// @brief Fixed keeps the static parent in place.
TEST_F(ConstraintTest, FixedKeepsTheStaticParentInPlace)
{
    physics::RigidBody parent = MakeStaticAt(math::Vector3::ZERO);
    physics::RigidBody child  = MakeDynamicAt({2.0f, 0.0f, 0.0f});
    physics::FixedConstraint constraint(&parent, &child);

    child.SetPosition({50.0f, 50.0f, 50.0f});
    constraint.SolvePosition(testkit::kFixedDeltaTime);

    EXPECT_VEC3_NEAR(parent.GetPosition(), math::Vector3::ZERO, testkit::kTolerance);
}

/// @name ヒンジ

/// @brief Hinge brings both anchors to the same world point.
TEST_F(ConstraintTest, HingeBringsBothAnchorsToTheSameWorldPoint)
{
    physics::RigidBody door  = MakeDynamicAt({5.0f, 0.0f, 0.0f});
    physics::RigidBody frame = MakeStaticAt(math::Vector3::ZERO);
    physics::HingeConstraint constraint(&frame, &door, {1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f},
                                        math::Vector3::UP);

    constraint.SolvePosition(testkit::kFixedDeltaTime);

    const math::Vector3 anchorFrame = frame.GetPosition() + frame.GetRotation() * math::Vector3(1.0f, 0.0f, 0.0f);
    const math::Vector3 anchorDoor  = door.GetPosition() + door.GetRotation() * math::Vector3(-1.0f, 0.0f, 0.0f);

    EXPECT_VEC3_NEAR(anchorDoor, anchorFrame, testkit::kTolerance);
}

/// @brief Hinge leaves the rotation alone when no limit is set.
TEST_F(ConstraintTest, HingeLeavesTheRotationAloneWhenNoLimitIsSet)
{
    physics::RigidBody frame = MakeStaticAt(math::Vector3::ZERO);
    physics::RigidBody door  = MakeDynamicAt({2.0f, 0.0f, 0.0f});
    const math::Quaternion opened =
        math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(80.0f));
    door.SetRotation(opened);
    physics::HingeConstraint constraint(&frame, &door, {1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f},
                                        math::Vector3::UP);

    constraint.SolvePosition(testkit::kFixedDeltaTime);

    EXPECT_QUAT_NEAR(door.GetRotation(), opened, testkit::kTolerance);
}

/// @brief Hinge rotates the body back inside its angular limit.
TEST_F(ConstraintTest, HingeRotatesTheBodyBackInsideItsAngularLimit)
{
    physics::RigidBody frame = MakeStaticAt(math::Vector3::ZERO);
    physics::RigidBody door  = MakeDynamicAt({2.0f, 0.0f, 0.0f});
    physics::HingeConstraint constraint(&frame, &door, {1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f},
                                        math::Vector3::UP);
    constraint.SetLimits(math::ToRad(-10.0f), math::ToRad(10.0f));

    door.SetRotation(math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(60.0f)));
    constraint.SolvePosition(testkit::kFixedDeltaTime);

    /// @note 制限まで戻す。行き過ぎて逆側へ回してはいけない。
    const math::Vector3 swung = door.GetRotation() * math::Vector3::FORWARD;
    const float angle = math::ToDeg(std::acos(
        math::Clamp(math::Vector3::Dot(swung, math::Vector3::FORWARD), -1.0f, 1.0f)));

    EXPECT_NEAR(angle, 10.0f, 0.5f);
}

/// @brief Checks hinge motor spins the driven body towards the target speed.
TEST_F(ConstraintTest, HingeMotorSpinsTheDrivenBodyTowardsTheTargetSpeed)
{
    physics::RigidBody frame = MakeStaticAt(math::Vector3::ZERO);
    physics::RigidBody door  = MakeDynamicAt({2.0f, 0.0f, 0.0f});
    physics::HingeConstraint constraint(&frame, &door, {1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f},
                                        math::Vector3::UP);
    constraint.SetMotor(5.0f, 100.0f);

    constraint.ApplyForce(testkit::kFixedDeltaTime);
    door.Integrate(testkit::kFixedDeltaTime);

    EXPECT_GT(door.GetAngularVelocity().y, 0.0f);
}

/// @brief Hinge applies no torque without a motor.
TEST_F(ConstraintTest, HingeAppliesNoTorqueWithoutAMotor)
{
    physics::RigidBody frame = MakeStaticAt(math::Vector3::ZERO);
    physics::RigidBody door  = MakeDynamicAt({2.0f, 0.0f, 0.0f});
    physics::HingeConstraint constraint(&frame, &door, {1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f},
                                        math::Vector3::UP);

    constraint.ApplyForce(testkit::kFixedDeltaTime);
    door.Integrate(testkit::kFixedDeltaTime);

    EXPECT_VEC3_NEAR(door.GetAngularVelocity(), math::Vector3::ZERO, testkit::kTolerance);
}

/// @brief Hinge stops driving after the motor is cleared.
TEST_F(ConstraintTest, HingeStopsDrivingAfterTheMotorIsCleared)
{
    physics::RigidBody frame = MakeStaticAt(math::Vector3::ZERO);
    physics::RigidBody door  = MakeDynamicAt({2.0f, 0.0f, 0.0f});
    physics::HingeConstraint constraint(&frame, &door, {1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f},
                                        math::Vector3::UP);
    constraint.SetMotor(5.0f, 100.0f);

    constraint.ClearMotor();
    constraint.ApplyForce(testkit::kFixedDeltaTime);
    door.Integrate(testkit::kFixedDeltaTime);

    EXPECT_VEC3_NEAR(door.GetAngularVelocity(), math::Vector3::ZERO, testkit::kTolerance);
}

} // namespace fbzz::tests
