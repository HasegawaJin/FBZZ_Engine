/// @file    Vector2Tests.cpp
/// @brief   Vector2 の長さ・正規化・内積・補間・誤差込み等値判定の契約を固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// UI とスクリーン座標が全部この型を通る。壊れると «少しずれた» 見た目だけが残り、
/// どのレイヤーの計算が狂ったのか最後まで分からなくなる。
#include <TestKit/TestKit.hpp>

#include <Math/MathUtils.hpp>
#include <Math/Vector2.hpp>

namespace fbzz::tests {

class Vector2Test : public testkit::Fixture {};

/// @name 長さと正規化

TEST_F(Vector2Test, LengthSquaredMatchesLengthSquared)
{
    const math::Vector2 v(3.0f, 4.0f);

    EXPECT_NEAR(v.LengthSq(), v.Length() * v.Length(), testkit::kTolerance);
    EXPECT_NEAR(v.Length(), 5.0f, testkit::kTolerance);
}

TEST_F(Vector2Test, NormalizedProducesUnitLength)
{
    for (int i = 0; i < 64; ++i) {
        const math::Vector2 v = Rng().NextVector2(-100.0f, 100.0f);
        if (v.LengthSq() < math::EPSILON) continue;

        EXPECT_NEAR(v.Normalized().Length(), 1.0f, testkit::kTolerance);
    }
}

TEST_F(Vector2Test, NormalizedPreservesDirection)
{
    const math::Vector2 v(0.0f, -7.5f);

    EXPECT_VEC2_NEAR(v.Normalized(), math::Vector2(0.0f, -1.0f), testkit::kTolerance);
}

/// @name 内積

TEST_F(Vector2Test, DotIsZeroForPerpendicularAxes)
{
    EXPECT_NEAR(math::Vector2::Dot({1.0f, 0.0f}, {0.0f, 1.0f}), 0.0f, testkit::kTolerance);
}

TEST_F(Vector2Test, DotIsCommutative)
{
    for (int i = 0; i < 64; ++i) {
        const math::Vector2 a = Rng().NextVector2(-10.0f, 10.0f);
        const math::Vector2 b = Rng().NextVector2(-10.0f, 10.0f);

        EXPECT_NEAR(math::Vector2::Dot(a, b), math::Vector2::Dot(b, a), testkit::kTolerance);
    }
}

TEST_F(Vector2Test, DotWithItselfEqualsLengthSquared)
{
    const math::Vector2 v = Rng().NextVector2(-10.0f, 10.0f);

    EXPECT_NEAR(math::Vector2::Dot(v, v), v.LengthSq(), testkit::kTolerance);
}

/// @name 補間と演算子

TEST_F(Vector2Test, LerpReturnsTheEndpointsAtZeroAndOne)
{
    const math::Vector2 a(1.0f, 2.0f);
    const math::Vector2 b(-4.0f, 0.5f);

    EXPECT_VEC2_NEAR(math::Vector2::Lerp(a, b, 0.0f), a, testkit::kTolerance);
    EXPECT_VEC2_NEAR(math::Vector2::Lerp(a, b, 1.0f), b, testkit::kTolerance);
}

TEST_F(Vector2Test, LerpAtMidpointIsTheAverage)
{
    const math::Vector2 a(0.0f, 0.0f);
    const math::Vector2 b(2.0f, -6.0f);

    EXPECT_VEC2_NEAR(math::Vector2::Lerp(a, b, 0.5f), math::Vector2(1.0f, -3.0f),
                     testkit::kTolerance);
}

TEST_F(Vector2Test, CompoundAssignmentMatchesTheBinaryOperator)
{
    const math::Vector2 a(1.0f, 2.0f);
    const math::Vector2 b(4.0f, -5.0f);

    math::Vector2 sum = a;
    sum += b;
    math::Vector2 difference = a;
    difference -= b;

    EXPECT_VEC2_NEAR(sum, a + b, testkit::kTolerance);
    EXPECT_VEC2_NEAR(difference, a - b, testkit::kTolerance);
}

TEST_F(Vector2Test, DivisionIsTheInverseOfScaling)
{
    const math::Vector2 v(3.0f, -8.0f);

    EXPECT_VEC2_NEAR((v * 4.0f) / 4.0f, v, testkit::kTolerance);
}

/// @name 等値判定

TEST_F(Vector2Test, EqualityToleratesDifferencesBelowEpsilon)
{
    /// @note operator== は誤差込みの比較。ビット一致ではないことを契約として固定する。
    const math::Vector2 a(0.0f, 0.0f);
    const math::Vector2 b(math::EPSILON * 0.5f, 0.0f);

    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a != b);
}

TEST_F(Vector2Test, EqualityRejectsDifferencesAboveEpsilon)
{
    const math::Vector2 a(0.0f, 0.0f);
    const math::Vector2 b(0.0f, math::EPSILON * 10.0f);

    EXPECT_FALSE(a == b);
    EXPECT_TRUE(a != b);
}

} // namespace fbzz::tests
