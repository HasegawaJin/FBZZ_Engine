/// @file    Matrix3Tests.cpp
/// @brief   Matrix3 の積・転置・Matrix4 からの切り出しが、行優先・列ベクトル規則どおりであることを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// この型は法線変換に使われる。行と列を取り違えても «転置された結果» が返るだけで
/// クラッシュせず、ライティングが微妙に合わない、という形でしか表面化しない。
#include <TestKit/TestKit.hpp>

#include <Math/MathUtils.hpp>
#include <Math/Matrix3.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::tests {
namespace {

/// 成分が全て異なる行列。転置と行列積の取り違えを検出できるように、対称にならない値を選ぶ。
math::Matrix3 MakeAsymmetric()
{
    math::Matrix3 mat;
    float value = 1.0f;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) mat.m[r][c] = value++;
    return mat;
}

} // namespace

class Matrix3Test : public testkit::Fixture {};

// --- 単位元と積 -------------------------------------------------------------

TEST_F(Matrix3Test, IdentityLeavesAMatrixUnchanged)
{
    const math::Matrix3 mat = MakeAsymmetric();

    EXPECT_MAT3_NEAR(mat * math::Matrix3::Identity(), mat, testkit::kTolerance);
    EXPECT_MAT3_NEAR(math::Matrix3::Identity() * mat, mat, testkit::kTolerance);
}

TEST_F(Matrix3Test, IdentityLeavesAVectorUnchanged)
{
    const math::Vector3 v = Rng().NextVector3(-10.0f, 10.0f);

    EXPECT_VEC3_NEAR(math::Matrix3::Identity() * v, v, testkit::kTolerance);
}

TEST_F(Matrix3Test, MultiplicationAppliesTheRightHandOperandFirst)
{
    // (A * B) * v == A * (B * v)。ここが逆だと変換の適用順が全部裏返る。
    const math::Matrix3 a = math::Matrix3::FromMatrix4(
        math::Matrix4::Rotate(math::Quaternion::FromAxisAngle(math::Vector3::UP,
                                                             math::ToRad(90.0f))));
    const math::Matrix3 b = math::Matrix3::FromMatrix4(
        math::Matrix4::Scale({2.0f, 3.0f, 4.0f}));
    const math::Vector3 v(1.0f, 1.0f, 1.0f);

    EXPECT_VEC3_NEAR((a * b) * v, a * (b * v), testkit::kTolerance);
}

TEST_F(Matrix3Test, MultiplicationIsAssociative)
{
    const math::Matrix3 a = MakeAsymmetric();
    const math::Matrix3 b = math::Matrix3::Transpose(MakeAsymmetric());
    const math::Matrix3 c = math::Matrix3::FromMatrix4(math::Matrix4::Scale({1.0f, 2.0f, 3.0f}));

    EXPECT_MAT3_NEAR((a * b) * c, a * (b * c), testkit::kTolerance);
}

// --- 転置 -------------------------------------------------------------------

TEST_F(Matrix3Test, TransposeSwapsRowsAndColumns)
{
    const math::Matrix3 mat = MakeAsymmetric();

    const math::Matrix3 transposed = math::Matrix3::Transpose(mat);

    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            EXPECT_NEAR(transposed.m[r][c], mat.m[c][r], testkit::kTolerance);
}

TEST_F(Matrix3Test, TransposeAppliedTwiceIsTheOriginal)
{
    const math::Matrix3 mat = MakeAsymmetric();

    EXPECT_MAT3_NEAR(math::Matrix3::Transpose(math::Matrix3::Transpose(mat)), mat,
                     testkit::kTolerance);
}

TEST_F(Matrix3Test, TransposeOfAProductReversesTheOperands)
{
    const math::Matrix3 a = MakeAsymmetric();
    const math::Matrix3 b = math::Matrix3::FromMatrix4(math::Matrix4::Scale({2.0f, -1.0f, 0.5f}));

    EXPECT_MAT3_NEAR(math::Matrix3::Transpose(a * b),
                     math::Matrix3::Transpose(b) * math::Matrix3::Transpose(a),
                     testkit::kTolerance);
}

TEST_F(Matrix3Test, TransposeOfARotationIsItsInverse)
{
    const math::Matrix3 rotation = math::Matrix3::FromMatrix4(
        math::Matrix4::Rotate(Rng().NextRotation()));

    EXPECT_MAT3_NEAR(rotation * math::Matrix3::Transpose(rotation), math::Matrix3::Identity(),
                     testkit::kTolerance);
}

// --- Matrix4 からの切り出し -------------------------------------------------

TEST_F(Matrix3Test, FromMatrix4TakesTheUpperLeftBlock)
{
    const math::Matrix4 mat = math::Matrix4::TRS({10.0f, -20.0f, 30.0f},
                                                 math::Quaternion::Identity(),
                                                 {2.0f, 3.0f, 4.0f});

    const math::Matrix3 linear = math::Matrix3::FromMatrix4(mat);

    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            EXPECT_NEAR(linear.m[r][c], mat.m[r][c], testkit::kTolerance);
}

TEST_F(Matrix3Test, FromMatrix4DiscardsTranslation)
{
    // 法線変換に使う型なので、平行移動を拾ってはならない。
    const math::Matrix4 translation = math::Matrix4::Translate({10.0f, -20.0f, 30.0f});

    EXPECT_MAT3_NEAR(math::Matrix3::FromMatrix4(translation), math::Matrix3::Identity(),
                     testkit::kTolerance);
}

TEST_F(Matrix3Test, RotatingAVectorMatchesTheEquivalentQuaternion)
{
    const math::Quaternion rotation =
        math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(90.0f));
    const math::Matrix3 mat = math::Matrix3::FromMatrix4(math::Matrix4::Rotate(rotation));
    const math::Vector3 v = Rng().NextVector3(-10.0f, 10.0f);

    EXPECT_VEC3_NEAR(mat * v, rotation * v, testkit::kTolerance);
}

} // namespace fbzz::tests
