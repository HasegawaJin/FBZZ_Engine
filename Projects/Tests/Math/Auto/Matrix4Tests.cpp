/// @file    Matrix4Tests.cpp
/// @brief   Matrix4 の合成順・逆行列・投影行列が DirectX 左手系の規約どおりであることを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// TRS の適用順と投影の深度範囲 (0..1) は、間違っていても «なんとなく描ける» ため
/// 目視では気づけない。ここで数値として固定する。
#include <TestKit/TestKit.hpp>

#include <Math/MathUtils.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>

namespace fbzz::tests {

namespace {

math::Vector3 TransformPoint(const math::Matrix4& m, const math::Vector3& p)
{
    const math::Vector4 r = m * math::Vector4(p, 1.0f);
    return {r.x, r.y, r.z};
}

math::Vector3 TransformDirection(const math::Matrix4& m, const math::Vector3& d)
{
    const math::Vector4 r = m * math::Vector4(d, 0.0f);
    return {r.x, r.y, r.z};
}

} // namespace

class Matrix4Test : public testkit::Fixture {};

// --- 基本の変換 -------------------------------------------------------------

TEST_F(Matrix4Test, IdentityLeavesPointsUnchanged)
{
    const math::Vector3 p(1.0f, -2.0f, 3.0f);

    EXPECT_VEC3_NEAR(TransformPoint(math::Matrix4::Identity(), p), p, testkit::kTolerance);
}

TEST_F(Matrix4Test, TranslateMovesPointsButNotDirections)
{
    const math::Matrix4 m = math::Matrix4::Translate(math::Vector3(10.0f, 0.0f, -5.0f));
    const math::Vector3 v(1.0f, 1.0f, 1.0f);

    EXPECT_VEC3_NEAR(TransformPoint(m, v), math::Vector3(11.0f, 1.0f, -4.0f), testkit::kTolerance);
    EXPECT_VEC3_NEAR(TransformDirection(m, v), v, testkit::kTolerance);
}

TEST_F(Matrix4Test, ScaleMultipliesEachAxisIndependently)
{
    const math::Matrix4 m = math::Matrix4::Scale(math::Vector3(2.0f, 3.0f, -1.0f));

    EXPECT_VEC3_NEAR(TransformPoint(m, math::Vector3(1.0f, 1.0f, 1.0f)),
                     math::Vector3(2.0f, 3.0f, -1.0f), testkit::kTolerance);
}

TEST_F(Matrix4Test, RotateAgreesWithTheQuaternionItWasBuiltFrom)
{
    for (int i = 0; i < 32; ++i) {
        const math::Quaternion q = Rng().NextRotation();
        const math::Vector3    v = Rng().NextVector3(-5.0f, 5.0f);

        EXPECT_VEC3_NEAR(TransformDirection(math::Matrix4::Rotate(q), v), q * v,
                         testkit::kLooseTolerance);
    }
}

// --- 合成順 -----------------------------------------------------------------

TEST_F(Matrix4Test, TRSAppliesScaleThenRotationThenTranslation)
{
    const math::Vector3    t(5.0f, 0.0f, 0.0f);
    const math::Quaternion r = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::HALF_PI);
    const math::Vector3    s(2.0f, 2.0f, 2.0f);

    const math::Vector3 point(1.0f, 0.0f, 0.0f);

    // 期待: (1,0,0) -> スケールで (2,0,0) -> Y 回りに 90 度で (0,0,-2) -> 平行移動で (5,0,-2)
    EXPECT_VEC3_NEAR(TransformPoint(math::Matrix4::TRS(t, r, s), point),
                     t + (r * (point * 2.0f)), testkit::kLooseTolerance);
}

TEST_F(Matrix4Test, MultiplicationIsAssociative)
{
    const math::Matrix4 a = math::Matrix4::Translate(Rng().NextVector3(-5.0f, 5.0f));
    const math::Matrix4 b = math::Matrix4::Rotate(Rng().NextRotation());
    const math::Matrix4 c = math::Matrix4::Scale(math::Vector3(1.5f, 2.0f, 0.5f));

    EXPECT_MAT4_NEAR((a * b) * c, a * (b * c), testkit::kLooseTolerance);
}

TEST_F(Matrix4Test, MultiplicationAppliesTheRightOperandFirst)
{
    const math::Matrix4 translate = math::Matrix4::Translate(math::Vector3(1.0f, 0.0f, 0.0f));
    const math::Matrix4 scale     = math::Matrix4::Scale(math::Vector3(2.0f, 2.0f, 2.0f));
    const math::Vector3 point(1.0f, 0.0f, 0.0f);

    // Translate * Scale なら «拡大してから移動» で (3,0,0)。逆順なら (4,0,0) になる。
    EXPECT_VEC3_NEAR(TransformPoint(translate * scale, point), math::Vector3(3.0f, 0.0f, 0.0f),
                     testkit::kTolerance);
}

// --- 逆行列と転置 -----------------------------------------------------------

TEST_F(Matrix4Test, InverseTimesOriginalIsIdentity)
{
    // 引数の評価順は未規定なので、乱数は必ず別の文で取り出す。
    const math::Vector3    translation = Rng().NextVector3(-5.0f, 5.0f);
    const math::Quaternion rotation    = Rng().NextRotation();
    const math::Matrix4    m = math::Matrix4::TRS(translation, rotation,
                                                  math::Vector3(2.0f, 0.5f, 1.5f));

    EXPECT_MAT4_NEAR(m * math::Matrix4::Inverse(m), math::Matrix4::Identity(),
                     testkit::kLooseTolerance);
}

TEST_F(Matrix4Test, TransposeIsItsOwnInverseOperation)
{
    const math::Vector3    translation = Rng().NextVector3(-5.0f, 5.0f);
    const math::Quaternion rotation    = Rng().NextRotation();
    const math::Matrix4    m = math::Matrix4::TRS(translation, rotation,
                                                  math::Vector3(1.0f, 2.0f, 3.0f));

    EXPECT_MAT4_NEAR(m.Transposed().Transposed(), m, testkit::kTolerance);
}

TEST_F(Matrix4Test, InverseTransposeAffineMatchesTheUpperLeftOfTheFullInverseTranspose)
{
    const math::Quaternion rotation = Rng().NextRotation();
    const math::Matrix4    m = math::Matrix4::TRS(math::Vector3(3.0f, -1.0f, 7.0f), rotation,
                                                  math::Vector3(2.0f, 0.5f, 1.5f));

    const math::Matrix4 reference = math::Matrix4::Transpose(math::Matrix4::Inverse(m));
    const math::Matrix4 fast      = math::Matrix4::InverseTransposeAffine(m);

    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            EXPECT_NEAR(fast.m[row][col], reference.m[row][col], testkit::kLooseTolerance)
                << "m[" << row << "][" << col << "]";
        }
    }
}

TEST_F(Matrix4Test, InverseTransposeAffineReturnsIdentityForADegenerateScale)
{
    // スケール 0 はエディタ操作として普通に起きる。停止せず素通しする契約。
    const math::Matrix4 m = math::Matrix4::Scale(math::Vector3(1.0f, 0.0f, 1.0f));

    EXPECT_MAT4_NEAR(math::Matrix4::InverseTransposeAffine(m), math::Matrix4::Identity(),
                     testkit::kTolerance);
}

// --- ビューと投影 -----------------------------------------------------------

TEST_F(Matrix4Test, LookAtPlacesTheEyeAtTheViewOrigin)
{
    const math::Vector3 eye(0.0f, 0.0f, -5.0f);
    const math::Matrix4 view = math::Matrix4::LookAt(eye, math::Vector3::ZERO, math::Vector3::UP);

    EXPECT_VEC3_NEAR(TransformPoint(view, eye), math::Vector3::ZERO, testkit::kTolerance);
}

TEST_F(Matrix4Test, LookAtPutsTheTargetOnThePositiveZAxis)
{
    // 左手系なので «見ている先» はビュー空間の +Z。距離はそのまま残る。
    const math::Vector3 eye(0.0f, 0.0f, -5.0f);
    const math::Matrix4 view = math::Matrix4::LookAt(eye, math::Vector3::ZERO, math::Vector3::UP);

    EXPECT_VEC3_NEAR(TransformPoint(view, math::Vector3::ZERO), math::Vector3(0.0f, 0.0f, 5.0f),
                     testkit::kTolerance);
}

TEST_F(Matrix4Test, PerspectiveMapsTheNearPlaneToZeroAndTheFarPlaneToOne)
{
    // DirectX の深度範囲は 0..1。OpenGL の -1..1 と取り違えると近景だけが破綻する。
    constexpr float     kNear = 0.1f;
    constexpr float     kFar  = 100.0f;
    const math::Matrix4 proj  = math::Matrix4::Perspective(math::ToRad(60.0f), 16.0f / 9.0f,
                                                           kNear, kFar);

    const math::Vector4 atNear = proj * math::Vector4(0.0f, 0.0f, kNear, 1.0f);
    const math::Vector4 atFar  = proj * math::Vector4(0.0f, 0.0f, kFar, 1.0f);

    ASSERT_GT(atNear.w, testkit::kEpsilon);
    ASSERT_GT(atFar.w, testkit::kEpsilon);
    EXPECT_NEAR(atNear.z / atNear.w, 0.0f, testkit::kLooseTolerance);
    EXPECT_NEAR(atFar.z / atFar.w, 1.0f, testkit::kLooseTolerance);
}

TEST_F(Matrix4Test, PerspectiveDividesWByViewSpaceDepth)
{
    const math::Matrix4 proj =
        math::Matrix4::Perspective(math::ToRad(60.0f), 1.0f, 0.1f, 100.0f);

    const math::Vector4 clip = proj * math::Vector4(1.0f, 2.0f, 4.0f, 1.0f);

    EXPECT_NEAR(clip.w, 4.0f, testkit::kTolerance);
}

TEST_F(Matrix4Test, OrthographicMapsTheBoxCornersToTheNdcCube)
{
    const math::Matrix4 proj = math::Matrix4::Orthographic(-2.0f, 2.0f, -1.0f, 1.0f, 0.0f, 10.0f);

    EXPECT_VEC3_NEAR(TransformPoint(proj, math::Vector3(2.0f, 1.0f, 10.0f)),
                     math::Vector3(1.0f, 1.0f, 1.0f), testkit::kTolerance);
    EXPECT_VEC3_NEAR(TransformPoint(proj, math::Vector3(-2.0f, -1.0f, 0.0f)),
                     math::Vector3(-1.0f, -1.0f, 0.0f), testkit::kTolerance);
}

} // namespace fbzz::tests
