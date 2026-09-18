/// @file    ScalarParityTests.cpp
/// @brief   Math の製品実装 (SIMD) をスカラー参照実装と固定の入力で突き合わせる。
/// @author  Hasegawa Jin
/// @date    2026-09-19
/// @see Docs/design/math-simd.md §5 正しさの確かめ方
#include <TestKit/TestKit.hpp>

#include "../Reference/ScalarReference.hpp"

#include <Math/Frustum.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace fbzz::tests {
namespace {

constexpr int kCaseCount = 512;

/// @brief 行列積・M*v の相対許容誤差。加算の順序が変わるぶんの丸めだけを許す。
constexpr float kProductTolerance = 1.0e-5f;
/// @brief 逆行列の相対許容誤差。余因子の組み方が変わると桁落ちの出方が変わる。
constexpr float kInverseTolerance = 1.0e-4f;
/// @brief カリングの突き合わせから外す境界の幅 [m]。これより境界に近い入力は丸めで判定が反転しうる。
constexpr float kBoundaryMargin = 1.0e-3f;

/// @return 成分の絶対値の最大 (1 未満なら 1)。相対誤差の分母に使う。
float Magnitude(const math::Matrix4& m)
{
    float result = 1.0f;
    for (const auto& row : m.m)
        for (float value : row) result = std::max(result, std::fabs(value));
    return result;
}

float Magnitude(const math::Vector4& v)
{
    return std::max({ 1.0f, std::fabs(v.x), std::fabs(v.y), std::fabs(v.z), std::fabs(v.w) });
}

math::Frustum PerspectiveFrustum()
{
    const math::Matrix4 view = math::Matrix4::LookAt({ 1.0f, 2.0f, -3.0f }, { 0.0f, 0.0f, 10.0f }, math::Vector3::UP);
    const math::Matrix4 proj = math::Matrix4::Perspective(math::ToRad(60.0f), 16.0f / 9.0f, 0.1f, 50.0f);
    return math::Frustum::FromViewProjection(proj * view);
}

math::Frustum OrthographicFrustum()
{
    const math::Matrix4 view = math::Matrix4::LookAt({ 0.0f, 20.0f, 0.0f }, math::Vector3::ZERO, math::Vector3::FORWARD);
    const math::Matrix4 proj = math::Matrix4::Orthographic(-15.0f, 15.0f, -10.0f, 10.0f, 0.0f, 40.0f);
    return math::Frustum::FromViewProjection(proj * view);
}

} // namespace

class ScalarParityTest : public testkit::Fixture {
protected:
    /// @brief 平行移動・回転・非一様スケールを持つアフィン行列と、成分が [-1, 1) の一般の行列を半々で並べる。
    std::vector<math::Matrix4> MakeMatrices()
    {
        std::vector<math::Matrix4> result;
        for (int i = 0; i < kCaseCount; ++i) {
            if (i % 2 == 0) {
                /// @note 引数の評価順は未規定なので、乱数は必ず別の文で取り出す。
                const math::Vector3    t = Rng().NextVector3(-10.0f, 10.0f);
                const math::Quaternion r = Rng().NextRotation();
                const math::Vector3    s = Rng().NextVector3(0.25f, 4.0f);
                result.push_back(math::Matrix4::TRS(t, r, s));
            } else {
                math::Matrix4 general;
                for (auto& row : general.m)
                    for (float& value : row) value = Rng().NextFloat(-1.0f, 1.0f);
                result.push_back(general);
            }
        }
        return result;
    }
};

/// @name Matrix4

TEST_F(ScalarParityTest, MatrixProductMatchesTheScalarReference)
{
    const std::vector<math::Matrix4> matrices = MakeMatrices();
    for (int i = 0; i < kCaseCount; ++i) {
        const math::Matrix4& a = matrices[i];
        const math::Matrix4& b = matrices[(i + 1) % kCaseCount];
        const math::Matrix4 expected = reference::Multiply(a, b);
        EXPECT_MAT4_NEAR(a * b, expected, kProductTolerance * Magnitude(expected)) << "case " << i;
    }
}

TEST_F(ScalarParityTest, MatrixVectorProductMatchesTheScalarReference)
{
    const std::vector<math::Matrix4> matrices = MakeMatrices();
    for (int i = 0; i < kCaseCount; ++i) {
        const math::Vector3 xyz = Rng().NextVector3(-20.0f, 20.0f);
        const math::Vector4 v{ xyz, Rng().NextFloat(-2.0f, 2.0f) };
        const math::Vector4 expected = reference::Multiply(matrices[i], v);
        EXPECT_VEC4_NEAR(matrices[i] * v, expected, kProductTolerance * Magnitude(expected)) << "case " << i;
    }
}

TEST_F(ScalarParityTest, TransposeMatchesTheScalarReference)
{
    /// @note 転置は値を動かすだけなので誤差 0 で一致する。
    for (const math::Matrix4& m : MakeMatrices())
        EXPECT_MAT4_NEAR(math::Matrix4::Transpose(m), reference::Transpose(m), 0.0f);
}

TEST_F(ScalarParityTest, InverseMatchesTheScalarReference)
{
    const std::vector<math::Matrix4> matrices = MakeMatrices();
    for (int i = 0; i < kCaseCount; ++i) {
        const math::Matrix4 expected = reference::Inverse(matrices[i]);
        EXPECT_MAT4_NEAR(math::Matrix4::Inverse(matrices[i]), expected,
                         kInverseTolerance * Magnitude(expected)) << "case " << i;
    }
}

TEST_F(ScalarParityTest, InverseOfASingularMatrixIsIdentity)
{
    /// @note scale に 0 が入った Transform は Inspector の操作で普通に起きる。契約は «単位行列を返す»。
    const math::Matrix4 flat = math::Matrix4::TRS({ 1.0f, 2.0f, 3.0f }, math::Quaternion::Identity(),
                                                  { 1.0f, 0.0f, 1.0f });
    EXPECT_MAT4_NEAR(math::Matrix4::Inverse(flat), math::Matrix4::Identity(), 0.0f);
}

TEST_F(ScalarParityTest, TrsMatchesTheProductOfTranslateRotateScale)
{
    for (int i = 0; i < kCaseCount; ++i) {
        const math::Vector3    t = Rng().NextVector3(-50.0f, 50.0f);
        const math::Quaternion r = Rng().NextRotation();
        /// @note 負のスケール (鏡映) も Transform では普通に使う。
        const math::Vector3    s = Rng().NextVector3(-4.0f, 4.0f);
        const math::Matrix4 expected = reference::Trs(t, r, s);
        EXPECT_MAT4_NEAR(math::Matrix4::TRS(t, r, s), expected,
                         kProductTolerance * Magnitude(expected)) << "case " << i;
    }
}

/// @name Frustum

TEST_F(ScalarParityTest, PointContainmentMatchesTheScalarReference)
{
    for (const math::Frustum& frustum : { PerspectiveFrustum(), OrthographicFrustum() }) {
        const reference::Planes planes = reference::PlanesOf(frustum);
        int inside = 0;
        int outside = 0;
        for (int i = 0; i < kCaseCount * 4; ++i) {
            const math::Vector3 point = Rng().NextVector3(-40.0f, 40.0f);
            const float margin = reference::SphereMargin(planes, point, 0.0f);
            if (std::fabs(margin) < kBoundaryMargin) continue;
            const bool expected = margin >= 0.0f;
            EXPECT_EQ(frustum.Contains(point), expected) << "case " << i;
            ++(expected ? inside : outside);
        }
        EXPECT_GT(inside, kCaseCount / 8);
        EXPECT_GT(outside, kCaseCount / 8);
    }
}

TEST_F(ScalarParityTest, SphereCullingMatchesTheScalarReference)
{
    for (const math::Frustum& frustum : { PerspectiveFrustum(), OrthographicFrustum() }) {
        const reference::Planes planes = reference::PlanesOf(frustum);
        int inside = 0;
        int outside = 0;
        for (int i = 0; i < kCaseCount * 4; ++i) {
            const math::Vector3 center = Rng().NextVector3(-40.0f, 40.0f);
            const float radius = Rng().NextFloat(0.0f, 5.0f);
            const float margin = reference::SphereMargin(planes, center, radius);
            if (std::fabs(margin) < kBoundaryMargin) continue;
            const bool expected = margin >= 0.0f;
            EXPECT_EQ(frustum.IntersectsSphere(center, radius), expected) << "case " << i;
            ++(expected ? inside : outside);
        }
        /// @note 内外の両方を十分に踏んでいること。入力の範囲が錐台から外れると片側しか検査しない。
        EXPECT_GT(inside, kCaseCount / 8);
        EXPECT_GT(outside, kCaseCount / 8);
    }
}

TEST_F(ScalarParityTest, AabbCullingMatchesTheScalarReference)
{
    for (const math::Frustum& frustum : { PerspectiveFrustum(), OrthographicFrustum() }) {
        const reference::Planes planes = reference::PlanesOf(frustum);
        int inside = 0;
        int outside = 0;
        for (int i = 0; i < kCaseCount * 4; ++i) {
            const math::Vector3 center = Rng().NextVector3(-40.0f, 40.0f);
            const math::Vector3 half = Rng().NextVector3(0.0f, 5.0f);
            const float margin = reference::AabbMargin(planes, center, half);
            if (std::fabs(margin) < kBoundaryMargin) continue;
            const bool expected = margin >= 0.0f;
            EXPECT_EQ(frustum.IntersectsAABB(center, half), expected) << "case " << i;
            ++(expected ? inside : outside);
        }
        EXPECT_GT(inside, kCaseCount / 8);
        EXPECT_GT(outside, kCaseCount / 8);
    }
}

} // namespace fbzz::tests
