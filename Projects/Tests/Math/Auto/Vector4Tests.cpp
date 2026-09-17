/// @file    Vector4Tests.cpp
/// @brief   Vector4 の同次座標としての振る舞いと、色定数のアルファ規約を固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// この型はシェーダー定数へそのまま流れる。BLACK の alpha が 0 になった、のような
/// 定数側の事故は絵にしか出ず、原因の特定に一番時間を食う。
#include <TestKit/TestKit.hpp>

#include <Math/MathUtils.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>

namespace fbzz::tests {

class Vector4Test : public testkit::Fixture {};

/// @name Vector3 との相互変換

TEST_F(Vector4Test, ConstructionFromVector3KeepsTheGivenW)
{
    const math::Vector3 v(1.0f, 2.0f, 3.0f);

    const math::Vector4 point(v, 1.0f);

    EXPECT_VEC4_NEAR(point, math::Vector4(1.0f, 2.0f, 3.0f, 1.0f), testkit::kTolerance);
}

TEST_F(Vector4Test, XYZDropsTheWComponent)
{
    const math::Vector4 v(1.0f, 2.0f, 3.0f, 99.0f);

    EXPECT_VEC3_NEAR(v.XYZ(), math::Vector3(1.0f, 2.0f, 3.0f), testkit::kTolerance);
}

TEST_F(Vector4Test, XYZRoundTripsThroughTheVector3Constructor)
{
    const math::Vector4 v(-4.0f, 0.5f, 8.0f, 1.0f);

    EXPECT_VEC4_NEAR(math::Vector4(v.XYZ(), v.w), v, testkit::kTolerance);
}

/// @name 長さと正規化

TEST_F(Vector4Test, LengthIncludesTheWComponent)
{
    /// @note w を落として計算していたら 1.0 になる。4 成分を見ていることを固定する。
    const math::Vector4 v(1.0f, 0.0f, 0.0f, 1.0f);

    EXPECT_NEAR(v.Length(), math::Sqrt(2.0f), testkit::kTolerance);
}

TEST_F(Vector4Test, NormalizedProducesUnitLength)
{
    const math::Vector4 v(2.0f, -3.0f, 6.0f, 4.0f);

    EXPECT_NEAR(v.Normalized().Length(), 1.0f, testkit::kTolerance);
}

TEST_F(Vector4Test, NormalizedPreservesTheComponentRatios)
{
    const math::Vector4 v(0.0f, 0.0f, 0.0f, -5.0f);

    EXPECT_VEC4_NEAR(v.Normalized(), math::Vector4(0.0f, 0.0f, 0.0f, -1.0f),
                     testkit::kTolerance);
}

/// @name 演算子

TEST_F(Vector4Test, AdditionAndSubtractionAreComponentWise)
{
    const math::Vector4 a(1.0f, 2.0f, 3.0f, 4.0f);
    const math::Vector4 b(5.0f, -6.0f, 7.0f, -8.0f);

    EXPECT_VEC4_NEAR(a + b, math::Vector4(6.0f, -4.0f, 10.0f, -4.0f), testkit::kTolerance);
    EXPECT_VEC4_NEAR(a - b, math::Vector4(-4.0f, 8.0f, -4.0f, 12.0f), testkit::kTolerance);
}

TEST_F(Vector4Test, ScalingByZeroYieldsZero)
{
    const math::Vector4 v(1.0f, 2.0f, 3.0f, 4.0f);

    EXPECT_VEC4_NEAR(v * 0.0f, math::Vector4::ZERO, testkit::kTolerance);
}

TEST_F(Vector4Test, EqualityToleratesDifferencesBelowEpsilon)
{
    const math::Vector4 a = math::Vector4::ZERO;
    const math::Vector4 b(0.0f, 0.0f, 0.0f, math::EPSILON * 0.5f);

    EXPECT_TRUE(a == b);
}

TEST_F(Vector4Test, EqualityRejectsDifferencesAboveEpsilon)
{
    const math::Vector4 a = math::Vector4::ZERO;
    const math::Vector4 b(0.0f, 0.0f, 0.0f, math::EPSILON * 10.0f);

    EXPECT_FALSE(a == b);
}

/// @name 色定数

TEST_F(Vector4Test, ColorConstantsAreOpaque)
{
    /// @note 色として使う定数は alpha = 1。ZERO / ONE は «数» なので対象外。
    EXPECT_NEAR(math::Vector4::WHITE.w, 1.0f, testkit::kTolerance);
    EXPECT_NEAR(math::Vector4::BLACK.w, 1.0f, testkit::kTolerance);
    EXPECT_NEAR(math::Vector4::RED.w, 1.0f, testkit::kTolerance);
    EXPECT_NEAR(math::Vector4::GREEN.w, 1.0f, testkit::kTolerance);
    EXPECT_NEAR(math::Vector4::BLUE.w, 1.0f, testkit::kTolerance);
}

TEST_F(Vector4Test, PrimaryColorConstantsIsolateASingleChannel)
{
    EXPECT_VEC3_NEAR(math::Vector4::RED.XYZ(), math::Vector3(1.0f, 0.0f, 0.0f),
                     testkit::kTolerance);
    EXPECT_VEC3_NEAR(math::Vector4::GREEN.XYZ(), math::Vector3(0.0f, 1.0f, 0.0f),
                     testkit::kTolerance);
    EXPECT_VEC3_NEAR(math::Vector4::BLUE.XYZ(), math::Vector3(0.0f, 0.0f, 1.0f),
                     testkit::kTolerance);
}

} // namespace fbzz::tests
