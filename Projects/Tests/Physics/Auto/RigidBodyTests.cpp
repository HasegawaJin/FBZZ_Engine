/// @file    RigidBodyTests.cpp
/// @brief   剛体の半陰的オイラー積分・力の消費・軸ロック・スリープ遷移の契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// «力はそのフレームだけ» «速度を先に更新してから位置を進める» という 2 つの前提が崩れると、
/// 落下距離が半フレームぶんずれる。絵では «少し重い» としか見えず、
/// 重力係数を触って辻褄を合わせてしまう類の壊れ方をする。
#include <TestKit/TestKit.hpp>

#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Physics/OBBCollider.hpp>
#include <Physics/RigidBody.hpp>
#include <Physics/SphereCollider.hpp>

namespace fbzz::tests {

class RigidBodyTest : public testkit::Fixture {
protected:
    physics::RigidBody body;
};

/// @name 積分

TEST_F(RigidBodyTest, IntegratesVelocityFromTheAccumulatedForce)
{
    body.SetMass(2.0f);
    body.ApplyForce({10.0f, 0.0f, 0.0f});

    body.Integrate(0.1f);

    EXPECT_VEC3_NEAR(body.GetVelocity(), math::Vector3(0.5f, 0.0f, 0.0f), testkit::kTolerance);
}

TEST_F(RigidBodyTest, AdvancesPositionWithTheUpdatedVelocity)
{
    /// @note 半陰的オイラー: 位置は «更新後» の速度で進む。陽的オイラーなら初速 0 のまま動かない。
    body.SetMass(1.0f);
    body.ApplyForce({10.0f, 0.0f, 0.0f});

    body.Integrate(0.1f);

    EXPECT_VEC3_NEAR(body.GetPosition(), math::Vector3(0.1f, 0.0f, 0.0f), testkit::kTolerance);
}

TEST_F(RigidBodyTest, ConsumesTheForceAfterASingleStep)
{
    /// @note 力は «そのフレームだけ» の入力。持ち越すと押し続けたように加速していく。
    body.SetMass(1.0f);
    body.ApplyForce({10.0f, 0.0f, 0.0f});
    body.Integrate(0.1f);

    const math::Vector3 afterFirst = body.GetVelocity();
    body.Integrate(0.1f);

    EXPECT_VEC3_NEAR(body.GetVelocity(), afterFirst, testkit::kTolerance);
}

TEST_F(RigidBodyTest, MovesAtConstantVelocityWithoutForces)
{
    body.SetVelocity({1.0f, 0.0f, 0.0f});

    testkit::StepFixed([this](float dt) { body.Integrate(dt); }, 60);

    EXPECT_VEC3_NEAR(body.GetPosition(), math::Vector3(1.0f, 0.0f, 0.0f), testkit::kLooseTolerance);
}

TEST_F(RigidBodyTest, LinearDragShrinksVelocityWithoutReversingIt)
{
    body.m_linearDrag = 5.0f;
    body.SetVelocity({10.0f, 0.0f, 0.0f});

    testkit::StepFixed([this](float dt) { body.Integrate(dt); }, 60);

    EXPECT_LT(body.GetVelocity().x, 10.0f);
    EXPECT_GT(body.GetVelocity().x, 0.0f);
}

TEST_F(RigidBodyTest, StaticBodiesDoNotIntegrate)
{
    body.m_isStatic = true;
    body.SetMass(1.0f);
    body.SetVelocity({10.0f, 0.0f, 0.0f});

    body.Integrate(0.1f);

    EXPECT_VEC3_NEAR(body.GetPosition(), math::Vector3::ZERO, testkit::kTolerance);
}

TEST_F(RigidBodyTest, KeepsTheRotationNormalizedWhileSpinning)
{
    /// @note q_dot の加算は正規性を壊す。放置すると回転行列がスケールを持ち、モデルが伸びる。
    body.SetAngularVelocity({0.0f, 10.0f, 0.0f});

    testkit::StepFixed([this](float dt) { body.Integrate(dt); }, 120);

    EXPECT_NEAR(body.GetRotation().Length(), 1.0f, testkit::kLooseTolerance);
}

/// @name 質量

TEST_F(RigidBodyTest, InverseMassIsTheReciprocalOfMass)
{
    body.SetMass(4.0f);

    EXPECT_NEAR(body.GetInvMass(), 0.25f, testkit::kTolerance);
    EXPECT_NEAR(body.GetMass(), 4.0f, testkit::kTolerance);
}

TEST_F(RigidBodyTest, ZeroMassMeansInfiniteMass)
{
    body.SetMass(0.0f);

    EXPECT_NEAR(body.GetInvMass(), 0.0f, testkit::kTolerance);
}

TEST_F(RigidBodyTest, StaticBodiesReportZeroInverseMass)
{
    body.m_isStatic = true;
    body.SetMass(1.0f);

    EXPECT_NEAR(body.GetInvMass(), 0.0f, testkit::kTolerance);
}

TEST_F(RigidBodyTest, NegativeMassAcceleratesAgainstTheAppliedForce)
{
    /// @note 反重力演出のために負の質量を許している。0 に丸めていないことを固定する。
    body.SetMass(-2.0f);
    body.ApplyForce({10.0f, 0.0f, 0.0f});

    body.Integrate(0.1f);

    EXPECT_LT(body.GetVelocity().x, 0.0f);
}

/// @name インパルス

TEST_F(RigidBodyTest, ImpulseChangesVelocityImmediately)
{
    body.SetMass(2.0f);

    body.ApplyImpulse({4.0f, 0.0f, 0.0f});

    EXPECT_VEC3_NEAR(body.GetVelocity(), math::Vector3(2.0f, 0.0f, 0.0f), testkit::kTolerance);
}

TEST_F(RigidBodyTest, ImpulseAtTheCenterOfMassDoesNotSpinTheBody)
{
    body.SetMass(1.0f);

    body.ApplyImpulseAtPoint({0.0f, 1.0f, 0.0f}, body.GetPosition());

    EXPECT_VEC3_NEAR(body.GetAngularVelocity(), math::Vector3::ZERO, testkit::kTolerance);
}

TEST_F(RigidBodyTest, ImpulseOffTheCenterOfMassSpinsTheBody)
{
    /// @note «端を殴れば回る»。重心へ入れると回らずに滑るだけになる。
    body.SetMass(1.0f);

    body.ApplyImpulseAtPoint({0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f});

    EXPECT_GT(body.GetAngularVelocity().LengthSq(), 0.0f);
    EXPECT_GT(body.GetVelocity().y, 0.0f);
}

TEST_F(RigidBodyTest, TorqueOnlyChangesTheAngularVelocity)
{
    body.SetMass(1.0f);
    body.ApplyTorque({0.0f, 1.0f, 0.0f});

    body.Integrate(0.1f);

    EXPECT_GT(body.GetAngularVelocity().y, 0.0f);
    EXPECT_VEC3_NEAR(body.GetVelocity(), math::Vector3::ZERO, testkit::kTolerance);
}

TEST_F(RigidBodyTest, ForceAtAPointAddsBothForceAndTorque)
{
    body.SetMass(1.0f);

    body.ApplyForceAtPoint({0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f});
    body.Integrate(0.1f);

    EXPECT_GT(body.GetVelocity().y, 0.0f);
    EXPECT_GT(body.GetAngularVelocity().LengthSq(), 0.0f);
}

/// @name 慣性

TEST_F(RigidBodyTest, InertiaFromASphereIsIsotropic)
{
    physics::SphereCollider sphere(1.0f);
    body.SetMass(1.0f);
    body.SetInertiaFromCollider(&sphere);

    const math::Vector3 x = body.ApplyInvInertia(math::Vector3::RIGHT);
    const math::Vector3 y = body.ApplyInvInertia(math::Vector3::UP);

    EXPECT_NEAR(x.Length(), y.Length(), testkit::kTolerance);
}

TEST_F(RigidBodyTest, InertiaFromABoxResistsTheLongAxisTheLeast)
{
    /// @note 細長い箱は «長い軸まわり» が一番回りやすい。ここが逆だと転倒の見た目が不自然になる。
    physics::OBBCollider box({0.5f, 4.0f, 0.5f});
    body.SetMass(1.0f);
    body.SetInertiaFromCollider(&box);

    const math::Vector3 aroundLong  = body.ApplyInvInertia(math::Vector3::UP);
    const math::Vector3 aroundShort = body.ApplyInvInertia(math::Vector3::RIGHT);

    EXPECT_GT(aroundLong.Length(), aroundShort.Length());
}

TEST_F(RigidBodyTest, StaticBodiesHaveNoInverseInertia)
{
    body.m_isStatic = true;
    body.SetMass(1.0f);

    EXPECT_VEC3_NEAR(body.ApplyInvInertia(math::Vector3::UP), math::Vector3::ZERO,
                     testkit::kTolerance);
}

/// @name 軸ロック

TEST_F(RigidBodyTest, FreezingAnAxisStopsMotionAlongIt)
{
    body.SetMass(1.0f);
    body.SetFreezePosition({false, true, false});

    body.ApplyForce({10.0f, 10.0f, 0.0f});
    body.Integrate(0.1f);

    EXPECT_NEAR(body.GetPosition().y, 0.0f, testkit::kTolerance);
    EXPECT_GT(body.GetPosition().x, 0.0f);
}

TEST_F(RigidBodyTest, FreezingAnAxisClearsExistingVelocityOnThatAxis)
{
    body.SetVelocity({1.0f, 1.0f, 1.0f});

    body.SetFreezePosition({true, false, false});

    EXPECT_NEAR(body.GetVelocity().x, 0.0f, testkit::kTolerance);
    EXPECT_NEAR(body.GetVelocity().z, 1.0f, testkit::kTolerance);
}

TEST_F(RigidBodyTest, FreezingRotationStopsSpinOnTheLockedAxisOnly)
{
    body.SetMass(1.0f);
    body.SetFreezeRotation({true, false, false});

    body.SetAngularVelocity({5.0f, 5.0f, 0.0f});

    EXPECT_NEAR(body.GetAngularVelocity().x, 0.0f, testkit::kTolerance);
    EXPECT_NEAR(body.GetAngularVelocity().y, 5.0f, testkit::kTolerance);
}

TEST_F(RigidBodyTest, SetPositionKeepsTheFrozenAxisWhereItWas)
{
    body.SetPosition({0.0f, 5.0f, 0.0f});
    body.SetFreezePosition({false, true, false});

    body.SetPosition({1.0f, 100.0f, 0.0f});

    EXPECT_VEC3_NEAR(body.GetPosition(), math::Vector3(1.0f, 5.0f, 0.0f), testkit::kTolerance);
}

/// @name スリープ

TEST_F(RigidBodyTest, FallsAsleepOnlyAfterStayingSlowForTheFullDuration)
{
    testkit::StepFixed([this](float dt) { body.UpdateSleepState(dt, 0.1f, 0.1f, 0.5f); }, 20);
    EXPECT_FALSE(body.IsSleeping());

    testkit::StepFixed([this](float dt) { body.UpdateSleepState(dt, 0.1f, 0.1f, 0.5f); }, 20);

    EXPECT_TRUE(body.IsSleeping());
}

TEST_F(RigidBodyTest, StaysAwakeWhileMoving)
{
    body.SetVelocity({10.0f, 0.0f, 0.0f});

    testkit::StepFixed([this](float dt) { body.UpdateSleepState(dt, 0.1f, 0.1f, 0.5f); }, 120);

    EXPECT_FALSE(body.IsSleeping());
}

TEST_F(RigidBodyTest, NeverSleepsWhenSleepingIsDisabled)
{
    body.m_allowSleeping = false;

    testkit::StepFixed([this](float dt) { body.UpdateSleepState(dt, 0.1f, 0.1f, 0.5f); }, 120);

    EXPECT_FALSE(body.IsSleeping());
}

TEST_F(RigidBodyTest, SleepingBodiesReportZeroInverseMass)
{
    /// @note ソルバーは «動かせない相手» として扱う。ここが 0 でないと寝た剛体が押し返される。
    body.SetMass(1.0f);

    body.Sleep();

    EXPECT_NEAR(body.GetInvMass(), 0.0f, testkit::kTolerance);
    EXPECT_VEC3_NEAR(body.ApplyInvInertia(math::Vector3::UP), math::Vector3::ZERO,
                     testkit::kTolerance);
}

TEST_F(RigidBodyTest, SleepClearsVelocity)
{
    body.SetVelocity({5.0f, 0.0f, 0.0f});
    body.SetAngularVelocity({0.0f, 5.0f, 0.0f});

    body.Sleep();

    EXPECT_VEC3_NEAR(body.GetVelocity(), math::Vector3::ZERO, testkit::kTolerance);
    EXPECT_VEC3_NEAR(body.GetAngularVelocity(), math::Vector3::ZERO, testkit::kTolerance);
}

TEST_F(RigidBodyTest, SleepingBodiesDoNotIntegrate)
{
    body.SetMass(1.0f);
    body.Sleep();

    body.ApplyForceNoWake({100.0f, 0.0f, 0.0f});
    body.Integrate(0.1f);

    EXPECT_VEC3_NEAR(body.GetPosition(), math::Vector3::ZERO, testkit::kTolerance);
}

TEST_F(RigidBodyTest, ApplyForceWakesASleepingBody)
{
    body.Sleep();

    body.ApplyForce({1.0f, 0.0f, 0.0f});

    EXPECT_FALSE(body.IsSleeping());
}

TEST_F(RigidBodyTest, GravityDoesNotWakeASleepingBody)
{
    /// @note ApplyForceNoWake が起こしてしまうと、接地した剛体が永久に眠れない。
    body.Sleep();

    body.ApplyForceNoWake({0.0f, -9.81f, 0.0f});

    EXPECT_TRUE(body.IsSleeping());
}

TEST_F(RigidBodyTest, StaticBodiesNeverSleep)
{
    body.m_isStatic = true;

    body.Sleep();

    EXPECT_FALSE(body.IsSleeping());
}

/// @name 回転の正規化

TEST_F(RigidBodyTest, SetRotationNormalizesTheGivenQuaternion)
{
    body.SetRotation({0.0f, 0.0f, 0.0f, 4.0f});

    EXPECT_NEAR(body.GetRotation().Length(), 1.0f, testkit::kTolerance);
}

} // namespace fbzz::tests
