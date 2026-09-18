/// @file    QuaternionTests.cpp
/// @brief   Quaternion の合成順・回転作用・補間・行列往復の契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// 回転は «少しずれている» が絵の上では気づきにくく、気づいたときには
/// アニメーション・IK・カメラのどこが原因か切り分けられなくなっている。
/// 合成順 (Qy*Qx*Qz) と «q と -q は同じ回転» の 2 つを特に固定する。
#include <TestKit/TestKit.hpp>

#include <Math/MathUtils.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::tests {

class QuaternionTest : public testkit::Fixture {};

/// @name 回転作用

TEST_F(QuaternionTest, IdentityLeavesVectorsUnchanged)
{
    const math::Vector3 v(1.0f, -2.0f, 3.0f);

    EXPECT_VEC3_NEAR(math::Quaternion::Identity() * v, v, testkit::kTolerance);
}

TEST_F(QuaternionTest, RotationAroundRightByNinetyDegreesMapsUpToForward)
{
    /// @note 左手系の基底の向きそのもの。反転するとキャラクターの前後が入れ替わる。
    const math::Quaternion q = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::HALF_PI);

    EXPECT_VEC3_NEAR(q * math::Vector3::UP, math::Vector3::FORWARD, testkit::kTolerance);
}

TEST_F(QuaternionTest, RotationLeavesTheAxisItself)
{
    const math::Vector3    axis = Rng().NextUnitVector3();
    const math::Quaternion q    = math::Quaternion::FromAxisAngle(axis, 1.2345f);

    EXPECT_VEC3_NEAR(q * axis, axis, testkit::kTolerance);
}

TEST_F(QuaternionTest, RotationPreservesLength)
{
    for (int i = 0; i < 32; ++i) {
        const math::Quaternion q = Rng().NextRotation();
        const math::Vector3    v = Rng().NextVector3(-10.0f, 10.0f);

        EXPECT_NEAR((q * v).Length(), v.Length(), testkit::kLooseTolerance);
    }
}

TEST_F(QuaternionTest, ConjugateUndoesTheRotationOfAUnitQuaternion)
{
    for (int i = 0; i < 32; ++i) {
        const math::Quaternion q = Rng().NextRotation();
        const math::Vector3    v = Rng().NextVector3(-5.0f, 5.0f);

        EXPECT_VEC3_NEAR(q.Conjugate() * (q * v), v, testkit::kLooseTolerance);
    }
}

TEST_F(QuaternionTest, InverseTimesOriginalIsIdentity)
{
    const math::Quaternion q = Rng().NextRotation();

    EXPECT_QUAT_NEAR(q * q.Inverse(), math::Quaternion::Identity(), testkit::kTolerance);
}

/// @name 合成

TEST_F(QuaternionTest, MultiplicationAppliesTheRightOperandFirst)
{
    const math::Quaternion a = Rng().NextRotation();
    const math::Quaternion b = Rng().NextRotation();
    const math::Vector3    v = Rng().NextVector3(-5.0f, 5.0f);

    EXPECT_VEC3_NEAR((a * b) * v, a * (b * v), testkit::kLooseTolerance);
}

TEST_F(QuaternionTest, MultiplicationIsNotCommutative)
{
    const math::Quaternion a = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::HALF_PI);
    const math::Quaternion b = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::HALF_PI);

    EXPECT_FALSE(a * b == b * a);
}

TEST_F(QuaternionTest, FromEulerComposesYawThenPitchThenRoll)
{
    /// @note YXZ 内因順 (Qy * Qx * Qz)。ここが変わるとエディタで入力した角度の意味が変わる。
    const math::Vector3 euler(0.3f, -0.7f, 1.1f);

    const math::Quaternion expected =
        math::Quaternion::FromAxisAngle(math::Vector3::UP, euler.y) *
        math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, euler.x) *
        math::Quaternion::FromAxisAngle(math::Vector3::FORWARD, euler.z);

    EXPECT_QUAT_NEAR(math::Quaternion::FromEuler(euler), expected, testkit::kTolerance);
}

/// @name 正規化

TEST_F(QuaternionTest, NormalizedProducesUnitLength)
{
    const math::Quaternion q(1.0f, 2.0f, 3.0f, 4.0f);

    EXPECT_NEAR(q.Normalized().Length(), 1.0f, testkit::kTolerance);
}

/// @name 補間

TEST_F(QuaternionTest, SlerpReturnsTheEndpointsAtZeroAndOne)
{
    const math::Quaternion a = Rng().NextRotation();
    const math::Quaternion b = Rng().NextRotation();

    EXPECT_QUAT_NEAR(math::Quaternion::Slerp(a, b, 0.0f), a, testkit::kLooseTolerance);
    EXPECT_QUAT_NEAR(math::Quaternion::Slerp(a, b, 1.0f), b, testkit::kLooseTolerance);
}

TEST_F(QuaternionTest, SlerpAtMidpointIsHalfTheAngle)
{
    const math::Quaternion a = math::Quaternion::Identity();
    const math::Quaternion b = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::HALF_PI);
    const math::Quaternion expected =
        math::Quaternion::FromAxisAngle(math::Vector3::UP, math::HALF_PI * 0.5f);

    EXPECT_QUAT_NEAR(math::Quaternion::Slerp(a, b, 0.5f), expected, testkit::kLooseTolerance);
}

TEST_F(QuaternionTest, SlerpTakesTheShorterArcWhenTheEndpointsFaceAway)
{
    /// @note b は a と同じ回転を表す «符号違い»。遠回りすると 1 回転ぶん余計に回る。
    const math::Quaternion a = math::Quaternion::FromAxisAngle(math::Vector3::UP, 0.2f);
    const math::Quaternion b{-a.x, -a.y, -a.z, -a.w};

    const math::Quaternion mid = math::Quaternion::Slerp(a, b, 0.5f);

    EXPECT_QUAT_NEAR(mid, a, testkit::kLooseTolerance);
}

TEST_F(QuaternionTest, LerpResultIsNormalized)
{
    const math::Quaternion a = Rng().NextRotation();
    const math::Quaternion b = Rng().NextRotation();

    EXPECT_NEAR(math::Quaternion::Lerp(a, b, 0.37f).Length(), 1.0f, testkit::kTolerance);
}

/// @name 行列との往復

TEST_F(QuaternionTest, RoundTripsThroughARotationMatrix)
{
    for (int i = 0; i < 32; ++i) {
        const math::Quaternion q = Rng().NextRotation();

        const math::Quaternion restored = math::Quaternion::FromMatrix4(math::Matrix4::Rotate(q));

        EXPECT_QUAT_NEAR(restored, q, testkit::kLooseTolerance);
    }
}

/// @name LookRotation

TEST_F(QuaternionTest, LookRotationAlignsForwardWithTheGivenDirection)
{
    for (int i = 0; i < 32; ++i) {
        const math::Vector3    direction = Rng().NextUnitVector3();
        const math::Quaternion q         = math::Quaternion::LookRotation(direction);

        EXPECT_VEC3_NEAR(q * math::Vector3::FORWARD, direction, testkit::kLooseTolerance);
    }
}

TEST_F(QuaternionTest, LookRotationReturnsIdentityForAZeroDirection)
{
    /// @note 追従対象へ重なった / 速度が 0 になった、は正常系に含まれる。
    EXPECT_QUAT_NEAR(math::Quaternion::LookRotation(math::Vector3::ZERO),
                     math::Quaternion::Identity(), testkit::kTolerance);
}

TEST_F(QuaternionTest, LookRotationStillAlignsForwardWhenTheDirectionIsParallelToUp)
{
    /// @note 真上を向く。既定の up と平行なので基底が作れず、代替 up に落ちる縮退ケース。
    const math::Quaternion q = math::Quaternion::LookRotation(math::Vector3::UP);

    EXPECT_VEC3_NEAR(q * math::Vector3::FORWARD, math::Vector3::UP, testkit::kLooseTolerance);
}

} // namespace fbzz::tests
