/// @file    Vector3Tests.cpp
/// @brief   Vector3 の正規化・内積・外積・等値判定が、左手座標系の定義どおりであることを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// 外積の向きと等値判定の «誤差込み» は、崩れても絵が少し歪むだけで気づきにくい。
/// ここが黙って狂うと、法線・接空間・カメラの基底が全部ずれる。
#include <TestKit/TestKit.hpp>

#include <Math/MathUtils.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::tests {

class Vector3Test : public testkit::Fixture {};

/// @name 長さと正規化

TEST_F(Vector3Test, LengthSquaredMatchesLengthSquared)
{
    const math::Vector3 v(3.0f, 4.0f, 12.0f);

    EXPECT_NEAR(v.LengthSq(), v.Length() * v.Length(), testkit::kTolerance);
    EXPECT_NEAR(v.Length(), 13.0f, testkit::kTolerance);
}

TEST_F(Vector3Test, NormalizedProducesUnitLength)
{
    for (int i = 0; i < 64; ++i) {
        const math::Vector3 v = Rng().NextVector3(-100.0f, 100.0f);
        if (v.LengthSq() < math::EPSILON) continue;

        EXPECT_UNIT_LENGTH(v.Normalized(), testkit::kTolerance);
    }
}

TEST_F(Vector3Test, NormalizedPreservesDirection)
{
    const math::Vector3 v(0.0f, 0.0f, 7.5f);

    EXPECT_VEC3_NEAR(v.Normalized(), math::Vector3::FORWARD, testkit::kTolerance);
}

TEST_F(Vector3Test, NormalizedOrReturnsTheFallbackForZeroLength)
{
    const math::Vector3 fallback(0.0f, 1.0f, 0.0f);

    EXPECT_VEC3_NEAR(math::Vector3::ZERO.NormalizedOr(fallback), fallback, testkit::kTolerance);
}

TEST_F(Vector3Test, NormalizedOrNormalizesNonDegenerateInput)
{
    const math::Vector3 v(0.0f, -3.0f, 0.0f);

    EXPECT_VEC3_NEAR(v.NormalizedOr(math::Vector3::UP), math::Vector3(0.0f, -1.0f, 0.0f),
                     testkit::kTolerance);
}

/// @name 内積

TEST_F(Vector3Test, DotIsZeroForPerpendicularAxes)
{
    EXPECT_NEAR(math::Vector3::Dot(math::Vector3::RIGHT, math::Vector3::UP), 0.0f,
                testkit::kTolerance);
    EXPECT_NEAR(math::Vector3::Dot(math::Vector3::UP, math::Vector3::FORWARD), 0.0f,
                testkit::kTolerance);
}

TEST_F(Vector3Test, DotOfOppositeUnitVectorsIsMinusOne)
{
    const math::Vector3 v = Rng().NextUnitVector3();

    EXPECT_NEAR(math::Vector3::Dot(v, -v), -1.0f, testkit::kTolerance);
}

TEST_F(Vector3Test, DotIsCommutative)
{
    for (int i = 0; i < 64; ++i) {
        const math::Vector3 a = Rng().NextVector3(-10.0f, 10.0f);
        const math::Vector3 b = Rng().NextVector3(-10.0f, 10.0f);

        EXPECT_NEAR(math::Vector3::Dot(a, b), math::Vector3::Dot(b, a), testkit::kTolerance);
    }
}

/// @name 外積

TEST_F(Vector3Test, CrossOfRightAndUpIsForward)
{
    /// @note 基底の向きの定義そのもの。ここが反転すると法線・接空間・カメラ基底が全部裏返る。
    EXPECT_VEC3_NEAR(math::Vector3::Cross(math::Vector3::RIGHT, math::Vector3::UP),
                     math::Vector3::FORWARD, testkit::kTolerance);
    EXPECT_VEC3_NEAR(math::Vector3::Cross(math::Vector3::UP, math::Vector3::FORWARD),
                     math::Vector3::RIGHT, testkit::kTolerance);
    EXPECT_VEC3_NEAR(math::Vector3::Cross(math::Vector3::FORWARD, math::Vector3::RIGHT),
                     math::Vector3::UP, testkit::kTolerance);
}

TEST_F(Vector3Test, CrossIsAntiCommutative)
{
    for (int i = 0; i < 64; ++i) {
        const math::Vector3 a = Rng().NextVector3(-10.0f, 10.0f);
        const math::Vector3 b = Rng().NextVector3(-10.0f, 10.0f);

        EXPECT_VEC3_NEAR(math::Vector3::Cross(a, b), -math::Vector3::Cross(b, a),
                         testkit::kTolerance);
    }
}

TEST_F(Vector3Test, CrossIsPerpendicularToBothOperands)
{
    for (int i = 0; i < 64; ++i) {
        const math::Vector3 a = Rng().NextUnitVector3();
        const math::Vector3 b = Rng().NextUnitVector3();
        const math::Vector3 c = math::Vector3::Cross(a, b);
        /// @note 平行な組は直交性を語れない
        if (c.LengthSq() < math::EPSILON) continue;

        EXPECT_NEAR(math::Vector3::Dot(c, a), 0.0f, testkit::kTolerance);
        EXPECT_NEAR(math::Vector3::Dot(c, b), 0.0f, testkit::kTolerance);
    }
}

TEST_F(Vector3Test, CrossOfParallelVectorsIsZero)
{
    const math::Vector3 v = Rng().NextUnitVector3();

    EXPECT_VEC3_NEAR(math::Vector3::Cross(v, v * 3.0f), math::Vector3::ZERO, testkit::kTolerance);
}

/// @name 補間と演算子

TEST_F(Vector3Test, LerpReturnsTheEndpointsAtZeroAndOne)
{
    const math::Vector3 a(1.0f, 2.0f, 3.0f);
    const math::Vector3 b(-4.0f, 0.5f, 8.0f);

    EXPECT_VEC3_NEAR(math::Vector3::Lerp(a, b, 0.0f), a, testkit::kTolerance);
    EXPECT_VEC3_NEAR(math::Vector3::Lerp(a, b, 1.0f), b, testkit::kTolerance);
}

TEST_F(Vector3Test, LerpAtMidpointIsTheAverage)
{
    const math::Vector3 a(0.0f, 0.0f, 0.0f);
    const math::Vector3 b(2.0f, -6.0f, 10.0f);

    EXPECT_VEC3_NEAR(math::Vector3::Lerp(a, b, 0.5f), math::Vector3(1.0f, -3.0f, 5.0f),
                     testkit::kTolerance);
}

TEST_F(Vector3Test, CompoundAssignmentMatchesTheBinaryOperator)
{
    const math::Vector3 a(1.0f, 2.0f, 3.0f);
    const math::Vector3 b(4.0f, -5.0f, 6.0f);

    math::Vector3 sum = a;
    sum += b;
    math::Vector3 difference = a;
    difference -= b;

    EXPECT_VEC3_NEAR(sum, a + b, testkit::kTolerance);
    EXPECT_VEC3_NEAR(difference, a - b, testkit::kTolerance);
}

TEST_F(Vector3Test, NegationEqualsScalingByMinusOne)
{
    const math::Vector3 v = Rng().NextVector3(-10.0f, 10.0f);

    EXPECT_VEC3_NEAR(-v, v * -1.0f, testkit::kTolerance);
}

/// @name 等値判定

TEST_F(Vector3Test, EqualityToleratesDifferencesBelowEpsilon)
{
    /// @note operator== は誤差込みの比較。ビット一致ではないことを契約として固定する。
    const math::Vector3 a(0.0f, 0.0f, 0.0f);
    const math::Vector3 b(math::EPSILON * 0.5f, 0.0f, 0.0f);

    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a != b);
}

TEST_F(Vector3Test, EqualityRejectsDifferencesAboveEpsilon)
{
    const math::Vector3 a(0.0f, 0.0f, 0.0f);
    const math::Vector3 b(math::EPSILON * 10.0f, 0.0f, 0.0f);

    EXPECT_FALSE(a == b);
    EXPECT_TRUE(a != b);
}

} // namespace fbzz::tests
