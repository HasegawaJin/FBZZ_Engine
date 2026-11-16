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

#include <limits>

namespace fbzz::tests {

class Vector3Test : public testkit::Fixture {};

/// @name 長さと正規化

/// @brief (3, 4, 12) の長さは 13 で、長さの二乗と LengthSq の結果が許容誤差内で一致する。
TEST_F(Vector3Test, LengthSquaredMatchesLengthSquared)
{
    const math::Vector3 v(3.0f, 4.0f, 12.0f);

    EXPECT_NEAR(v.LengthSq(), v.Length() * v.Length(), testkit::kTolerance);
    EXPECT_NEAR(v.Length(), 13.0f, testkit::kTolerance);
}

/// @brief 零に近い入力を除いた固定シードのベクトルで、正規化後の長さが 1 になる。
TEST_F(Vector3Test, NormalizedProducesUnitLength)
{
    for (int i = 0; i < 64; ++i) {
        const math::Vector3 v = Rng().NextVector3(-100.0f, 100.0f);
        if (v.LengthSq() < math::EPSILON) continue;

        EXPECT_UNIT_LENGTH(v.Normalized(), testkit::kTolerance);
    }
}

/// @brief 正の Z 軸上の入力を正規化すると、エンジンの前方単位ベクトルと一致する。
TEST_F(Vector3Test, NormalizedPreservesDirection)
{
    const math::Vector3 v(0.0f, 0.0f, 7.5f);

    EXPECT_VEC3_NEAR(v.Normalized(), math::Vector3::FORWARD, testkit::kTolerance);
}

/// @brief 長さがゼロの場合は、呼び出し側が指定した代替ベクトルを返す。
TEST_F(Vector3Test, NormalizedOrReturnsTheFallbackForZeroLength)
{
    const math::Vector3 fallback(0.0f, 1.0f, 0.0f);

    EXPECT_VEC3_NEAR(math::Vector3::ZERO.NormalizedOr(fallback), fallback, testkit::kTolerance);
}

/// @brief 長さのある入力は、異なる向きの代替値を指定していても入力の向きのまま正規化される。
TEST_F(Vector3Test, NormalizedOrNormalizesNonDegenerateInput)
{
    const math::Vector3 v(0.0f, -3.0f, 0.0f);

    EXPECT_VEC3_NEAR(v.NormalizedOr(math::Vector3::UP), math::Vector3(0.0f, -1.0f, 0.0f),
                     testkit::kTolerance);
}

/// @name 内積

/// @brief 右と上、上と前という直交基底の組では内積が 0 になる。
TEST_F(Vector3Test, DotIsZeroForPerpendicularAxes)
{
    EXPECT_NEAR(math::Vector3::Dot(math::Vector3::RIGHT, math::Vector3::UP), 0.0f,
                testkit::kTolerance);
    EXPECT_NEAR(math::Vector3::Dot(math::Vector3::UP, math::Vector3::FORWARD), 0.0f,
                testkit::kTolerance);
}

/// @brief 同じ単位ベクトルを反転して内積を取ると -1 になる。
TEST_F(Vector3Test, DotOfOppositeUnitVectorsIsMinusOne)
{
    const math::Vector3 v = Rng().NextUnitVector3();

    EXPECT_NEAR(math::Vector3::Dot(v, -v), -1.0f, testkit::kTolerance);
}

/// @brief 符号が混じる成分でも、内積は入力の順序を入れ替えた結果と一致する。
TEST_F(Vector3Test, DotIsCommutative)
{
    for (int i = 0; i < 64; ++i) {
        const math::Vector3 a = Rng().NextVector3(-10.0f, 10.0f);
        const math::Vector3 b = Rng().NextVector3(-10.0f, 10.0f);

        EXPECT_NEAR(math::Vector3::Dot(a, b), math::Vector3::Dot(b, a), testkit::kTolerance);
    }
}

/// @name 外積

/// @brief 右 × 上、上 × 前、前 × 右の外積がそれぞれ前、右、上の基底と一致する。
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

/// @brief 外積の左右を入れ替えると、各成分の符号が反転する。
TEST_F(Vector3Test, CrossIsAntiCommutative)
{
    for (int i = 0; i < 64; ++i) {
        const math::Vector3 a = Rng().NextVector3(-10.0f, 10.0f);
        const math::Vector3 b = Rng().NextVector3(-10.0f, 10.0f);

        EXPECT_VEC3_NEAR(math::Vector3::Cross(a, b), -math::Vector3::Cross(b, a),
                         testkit::kTolerance);
    }
}

/// @brief 平行でない単位ベクトルの外積は、両方の入力との内積が 0 になる。
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

/// @brief 同じ向きで長さだけが異なる二つのベクトルの外積はゼロベクトルになる。
TEST_F(Vector3Test, CrossOfParallelVectorsIsZero)
{
    const math::Vector3 v = Rng().NextUnitVector3();

    EXPECT_VEC3_NEAR(math::Vector3::Cross(v, v * 3.0f), math::Vector3::ZERO, testkit::kTolerance);
}

/// @name 補間と演算子

/// @brief 補間係数 0 は始点、1 は終点を成分ごとに再現する。
TEST_F(Vector3Test, LerpReturnsTheEndpointsAtZeroAndOne)
{
    const math::Vector3 a(1.0f, 2.0f, 3.0f);
    const math::Vector3 b(-4.0f, 0.5f, 8.0f);

    EXPECT_VEC3_NEAR(math::Vector3::Lerp(a, b, 0.0f), a, testkit::kTolerance);
    EXPECT_VEC3_NEAR(math::Vector3::Lerp(a, b, 1.0f), b, testkit::kTolerance);
}

/// @brief 補間係数 0.5 の結果は、負の成分も含めて各成分の平均になる。
TEST_F(Vector3Test, LerpAtMidpointIsTheAverage)
{
    const math::Vector3 a(0.0f, 0.0f, 0.0f);
    const math::Vector3 b(2.0f, -6.0f, 10.0f);

    EXPECT_VEC3_NEAR(math::Vector3::Lerp(a, b, 0.5f), math::Vector3(1.0f, -3.0f, 5.0f),
                     testkit::kTolerance);
}

/// @brief 加減算の複合代入は、同じ二つのベクトルに二項演算子を適用した結果と一致する。
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

/// @brief 単項マイナスは、各成分を -1 倍した結果と一致する。
TEST_F(Vector3Test, NegationEqualsScalingByMinusOne)
{
    const math::Vector3 v = Rng().NextVector3(-10.0f, 10.0f);

    EXPECT_VEC3_NEAR(-v, v * -1.0f, testkit::kTolerance);
}

/// @name 等値判定

/// @brief 一成分の差が EPSILON の半分なら等値と判定し、不等値は偽を返す。
TEST_F(Vector3Test, EqualityToleratesDifferencesBelowEpsilon)
{
    /// @note operator== は誤差込みの比較。ビット一致ではないことを契約として固定する。
    const math::Vector3 a(0.0f, 0.0f, 0.0f);
    const math::Vector3 b(math::EPSILON * 0.5f, 0.0f, 0.0f);

    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a != b);
}

/// @brief 一成分の差が EPSILON の 10 倍なら等値判定は偽、不等値判定は真を返す。
TEST_F(Vector3Test, EqualityRejectsDifferencesAboveEpsilon)
{
    const math::Vector3 a(0.0f, 0.0f, 0.0f);
    const math::Vector3 b(math::EPSILON * 10.0f, 0.0f, 0.0f);

    EXPECT_FALSE(a == b);
    EXPECT_TRUE(a != b);
}

/// @brief 近似比較は NaN を拒否し、許容誤差が無限大でも無限大の成分を拒否する。
TEST_F(Vector3Test, ApproximateAssertionsRejectNonFiniteComponents)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    const auto compareWithZero = [](const math::Vector3& actual, float tolerance) {
        return testkit::CompareVector3("actual", "zero", "tolerance", actual, math::Vector3::ZERO, tolerance);
    };
    EXPECT_FALSE(compareWithZero({0.0f, nan, 0.0f}, testkit::kTolerance));
    EXPECT_FALSE(compareWithZero({infinity, 0.0f, 0.0f}, infinity));
    EXPECT_TRUE(compareWithZero(math::Vector3::ZERO, testkit::kTolerance));
}

} /// @note namespace fbzz::tests
