/// @file    TransformTests.cpp
/// @brief   Transform のローカル / ワールド操作 (Translate・Rotate・LookAt・行列生成) を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// local と world の 2 系統を持ち、world は TransformSystem が毎フレーム作り直す。
/// «どちらを読み書きするか» を取り違えても、親を持たないオブジェクトでは両者が一致して
/// しまうため気づけない。親の下に置いた瞬間だけ壊れる、という形で出る。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Scene/Transform.hpp>

#include <Math/MathUtils.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::tests {
namespace {

/// 90 度だけ回した親の下に居る子、という状況を作る。
/// 親子の実体は GameObject 側が持つので、Transform 単体では
/// «world = 親 × local» の関係を手で用意する。
math::Quaternion QuarterTurnAboutUp()
{
    return math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(90.0f));
}

} // namespace

class TransformTest : public testkit::EngineFixture {
protected:
    scene::Transform transform;
};

/// @name 既定値

/// @brief Starts at the origin with unit scale.
TEST_F(TransformTest, StartsAtTheOriginWithUnitScale)
{
    EXPECT_VEC3_NEAR(transform.position, math::Vector3::ZERO, testkit::kTolerance);
    EXPECT_VEC3_NEAR(transform.scale, math::Vector3::ONE, testkit::kTolerance);
    EXPECT_QUAT_NEAR(transform.rotation, math::Quaternion::Identity(), testkit::kTolerance);
    EXPECT_VEC3_NEAR(transform.worldScale, math::Vector3::ONE, testkit::kTolerance);
}

/// @name 基底ベクトル

/// @brief Checks basis vectors come from the world rotation.
TEST_F(TransformTest, BasisVectorsComeFromTheWorldRotation)
{
    /// @note ローカル回転ではなくワールド回転から作る。親の回転を無視すると、
    ///       子の «前» が親の回転を無視した向きになる。
    transform.rotation      = math::Quaternion::Identity();
    transform.worldRotation = QuarterTurnAboutUp();

    EXPECT_VEC3_NEAR(transform.Forward(), math::Vector3::RIGHT, testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(transform.Up(), math::Vector3::UP, testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(transform.Right(), -math::Vector3::FORWARD, testkit::kLooseTolerance);
}

/// @brief Basis vectors stay orthonormal.
TEST_F(TransformTest, BasisVectorsStayOrthonormal)
{
    transform.worldRotation = Rng().NextRotation();

    EXPECT_UNIT_LENGTH(transform.Forward(), testkit::kLooseTolerance);
    EXPECT_NEAR(math::Vector3::Dot(transform.Forward(), transform.Up()), 0.0f,
                testkit::kLooseTolerance);
    EXPECT_NEAR(math::Vector3::Dot(transform.Up(), transform.Right()), 0.0f,
                testkit::kLooseTolerance);
}

/// @name Translate

/// @brief Translate moves along the local axes by default.
TEST_F(TransformTest, TranslateMovesAlongTheLocalAxesByDefault)
{
    transform.rotation = QuarterTurnAboutUp();

    transform.Translate(math::Vector3::FORWARD);

    EXPECT_VEC3_NEAR(transform.position, math::Vector3::RIGHT, testkit::kLooseTolerance);
}

/// @brief World-space translate ignores the own rotation.
TEST_F(TransformTest, WorldSpaceTranslateIgnoresTheOwnRotation)
{
    /// @note 親を持たないなら world 移動は «そのままの向き»。自分の回転で曲がってはいけない。
    transform.rotation      = QuarterTurnAboutUp();
    transform.worldRotation = QuarterTurnAboutUp();

    transform.Translate(math::Vector3::FORWARD, true);

    EXPECT_VEC3_NEAR(transform.position, math::Vector3::FORWARD, testkit::kLooseTolerance);
}

/// @brief Checks world-space translate goes through the parent rotation.
TEST_F(TransformTest, WorldSpaceTranslateGoesThroughTheParentRotation)
{
    /// @note 親が 90 度回っている子。world で +Z へ動かすと、ローカルでは親の逆回転が掛かる。
    transform.rotation      = math::Quaternion::Identity();
    /// @note world = 親 × local = 親
    transform.worldRotation = QuarterTurnAboutUp();

    transform.Translate(math::Vector3::FORWARD, true);

    EXPECT_VEC3_NEAR(transform.position, -math::Vector3::RIGHT, testkit::kLooseTolerance);
}

/// @brief Translate accumulates.
TEST_F(TransformTest, TranslateAccumulates)
{
    transform.Translate({ 1.0f, 0.0f, 0.0f });
    transform.Translate({ 0.0f, 2.0f, 0.0f });

    EXPECT_VEC3_NEAR(transform.position, math::Vector3(1.0f, 2.0f, 0.0f), testkit::kTolerance);
}

/// @name Rotate

/// @brief A Y-axis rotation input of 90 produces a quarter-turn about the up axis.
TEST_F(TransformTest, RotateTakesDegrees)
{
    transform.Rotate({ 0.0f, 90.0f, 0.0f });

    EXPECT_QUAT_NEAR(transform.rotation, QuarterTurnAboutUp(), testkit::kLooseTolerance);
}

/// @brief Rotate accumulates and stays normalised.
TEST_F(TransformTest, RotateAccumulatesAndStaysNormalised)
{
    for (int i = 0; i < 36; ++i) transform.Rotate({ 0.0f, 10.0f, 0.0f });

    EXPECT_NEAR(transform.rotation.Length(), 1.0f, testkit::kLooseTolerance);
    EXPECT_QUAT_NEAR(transform.rotation, math::Quaternion::Identity(), testkit::kLooseTolerance);
}

/// @brief Local rotate composes on the right hand side.
TEST_F(TransformTest, LocalRotateComposesOnTheRightHandSide)
{
    /// @note ローカル回転は «自分の軸で» 回る。yaw した後に pitch すると、傾いた軸で持ち上がる。
    transform.rotation = QuarterTurnAboutUp();
    transform.Rotate({ 90.0f, 0.0f, 0.0f });

    const math::Quaternion expected =
        QuarterTurnAboutUp() *
        math::Quaternion::FromEuler({ math::ToRad(90.0f), 0.0f, 0.0f });

    EXPECT_QUAT_NEAR(transform.rotation, expected, testkit::kLooseTolerance);
}

/// @name LookAt

/// @brief Checks look at points forward at the target.
TEST_F(TransformTest, LookAtPointsForwardAtTheTarget)
{
    transform.LookAt({ 10.0f, 0.0f, 0.0f });
    /// @note 親が居ないので local == world
    transform.worldRotation = transform.rotation;

    EXPECT_VEC3_NEAR(transform.Forward(), math::Vector3::RIGHT, testkit::kLooseTolerance);
}

/// @brief Checks look at measures from the world position.
TEST_F(TransformTest, LookAtMeasuresFromTheWorldPosition)
{
    transform.worldPosition = { 0.0f, 0.0f, 10.0f };

    transform.LookAt({ 0.0f, 0.0f, 20.0f });
    transform.worldRotation = transform.rotation;

    EXPECT_VEC3_NEAR(transform.Forward(), math::Vector3::FORWARD, testkit::kLooseTolerance);
}

/// @brief Checks look at ignores a target on top of itself.
TEST_F(TransformTest, LookAtIgnoresATargetOnTopOfItself)
{
    /// @note 追従対象と重なるのは普通に起きる。向きが決まらないだけで、落としてはいけない。
    transform.rotation = QuarterTurnAboutUp();

    transform.LookAt(transform.worldPosition);

    EXPECT_QUAT_NEAR(transform.rotation, QuarterTurnAboutUp(), testkit::kTolerance);
}

/// @name ワールド行列

/// @brief World matrix is built from the world values.
TEST_F(TransformTest, WorldMatrixIsBuiltFromTheWorldValues)
{
    /// @note 行列はローカル値ではなくワールド値から作る。ここを間違えると
    ///       親の下に置いた瞬間だけ描画位置がずれる。
    transform.position      = { 100.0f, 0.0f, 0.0f };
    transform.worldPosition = { 1.0f, 2.0f, 3.0f };
    transform.worldScale    = { 2.0f, 2.0f, 2.0f };

    const math::Matrix4 matrix = transform.GetWorldMatrix();

    EXPECT_MAT4_NEAR(matrix,
                     math::Matrix4::TRS({ 1.0f, 2.0f, 3.0f }, math::Quaternion::Identity(),
                                        { 2.0f, 2.0f, 2.0f }),
                     testkit::kTolerance);
}

/// @brief World matrix moves a point into world-space.
TEST_F(TransformTest, WorldMatrixMovesAPointIntoWorldSpace)
{
    transform.worldPosition = { 1.0f, 2.0f, 3.0f };
    transform.worldRotation = QuarterTurnAboutUp();
    transform.worldScale    = math::Vector3::ONE;

    const math::Vector4 moved =
        transform.GetWorldMatrix() * math::Vector4(math::Vector3::FORWARD, 1.0f);

    /// @note +Z が +X へ回り、そこから平行移動する。
    EXPECT_VEC3_NEAR(moved.XYZ(), math::Vector3(2.0f, 2.0f, 3.0f), testkit::kLooseTolerance);
}

} // namespace fbzz::tests
