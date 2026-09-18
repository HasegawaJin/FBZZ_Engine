/// @file    MathUtilsTests.cpp
/// @brief   スカラーユーティリティ (Clamp / Lerp / InverseLerp / Remap / 角度変換) の境界での振る舞いを固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// ここは全レイヤーが呼ぶ。境界 (t=0/1、区間幅 0、符号 0) の扱いが変わると、
/// 補間が 1 フレームだけ飛ぶ・ゲージが端で張り付く、という形で薄く広がる。
#include <TestKit/TestKit.hpp>

#include <Math/MathUtils.hpp>

namespace fbzz::tests {

class MathUtilsTest : public testkit::Fixture {};

/// @name 角度変換

TEST_F(MathUtilsTest, DegreeAndRadianConversionsAreInverses)
{
    for (int i = 0; i < 64; ++i) {
        const float deg = Rng().NextFloat(-720.0f, 720.0f);

        EXPECT_NEAR(math::ToDeg(math::ToRad(deg)), deg, 1.0e-3f);
    }
}

TEST_F(MathUtilsTest, HalfTurnInDegreesIsPiInRadians)
{
    EXPECT_NEAR(math::ToRad(180.0f), math::PI, testkit::kTolerance);
    EXPECT_NEAR(math::ToDeg(math::TWO_PI), 360.0f, 1.0e-3f);
}

/// @name Clamp

TEST_F(MathUtilsTest, ClampReturnsTheBoundsForOutOfRangeInput)
{
    EXPECT_NEAR(math::Clamp(-5.0f, 0.0f, 1.0f), 0.0f, testkit::kTolerance);
    EXPECT_NEAR(math::Clamp(5.0f, 0.0f, 1.0f), 1.0f, testkit::kTolerance);
}

TEST_F(MathUtilsTest, ClampLeavesInRangeInputUntouched)
{
    EXPECT_NEAR(math::Clamp(0.25f, 0.0f, 1.0f), 0.25f, testkit::kTolerance);
    EXPECT_NEAR(math::Clamp(-3.0f, -10.0f, 10.0f), -3.0f, testkit::kTolerance);
}

TEST_F(MathUtilsTest, ClampIncludesTheBoundsThemselves)
{
    EXPECT_NEAR(math::Clamp(0.0f, 0.0f, 1.0f), 0.0f, testkit::kTolerance);
    EXPECT_NEAR(math::Clamp(1.0f, 0.0f, 1.0f), 1.0f, testkit::kTolerance);
}

TEST_F(MathUtilsTest, Clamp01MatchesClampWithUnitBounds)
{
    for (int i = 0; i < 64; ++i) {
        const float v = Rng().NextFloat(-5.0f, 5.0f);

        EXPECT_NEAR(math::Clamp01(v), math::Clamp(v, 0.0f, 1.0f), testkit::kTolerance);
    }
}

/// @name 補間

TEST_F(MathUtilsTest, LerpReturnsTheEndpointsAtZeroAndOne)
{
    EXPECT_NEAR(math::Lerp(3.0f, 9.0f, 0.0f), 3.0f, testkit::kTolerance);
    EXPECT_NEAR(math::Lerp(3.0f, 9.0f, 1.0f), 9.0f, testkit::kTolerance);
}

TEST_F(MathUtilsTest, LerpExtrapolatesBeyondTheUnitInterval)
{
    /// @note t をクランプしない契約。カメラやゲージ側が «行き過ぎ» を自前で扱えるようにするため。
    EXPECT_NEAR(math::Lerp(0.0f, 10.0f, 2.0f), 20.0f, testkit::kTolerance);
    EXPECT_NEAR(math::Lerp(0.0f, 10.0f, -1.0f), -10.0f, testkit::kTolerance);
}

TEST_F(MathUtilsTest, InverseLerpUndoesLerp)
{
    for (int i = 0; i < 64; ++i) {
        const float t = Rng().NextFloat(0.0f, 1.0f);
        const float value = math::Lerp(-4.0f, 12.0f, t);

        EXPECT_NEAR(math::InverseLerp(-4.0f, 12.0f, value), t, 1.0e-4f);
    }
}

TEST_F(MathUtilsTest, InverseLerpReturnsZeroForAZeroWidthRange)
{
    /// @note 0 除算を避けるための既定値。ヘッダーに明記された契約そのもの。
    EXPECT_NEAR(math::InverseLerp(5.0f, 5.0f, 5.0f), 0.0f, testkit::kTolerance);
    EXPECT_NEAR(math::InverseLerp(5.0f, 5.0f, 100.0f), 0.0f, testkit::kTolerance);
}

TEST_F(MathUtilsTest, RemapMapsTheEndpointsOfBothRanges)
{
    EXPECT_NEAR(math::Remap(0.0f, 0.0f, 10.0f, 100.0f, 200.0f), 100.0f, testkit::kTolerance);
    EXPECT_NEAR(math::Remap(10.0f, 0.0f, 10.0f, 100.0f, 200.0f), 200.0f, testkit::kTolerance);
    EXPECT_NEAR(math::Remap(5.0f, 0.0f, 10.0f, 100.0f, 200.0f), 150.0f, testkit::kTolerance);
}

TEST_F(MathUtilsTest, RemapSupportsAnInvertedOutputRange)
{
    EXPECT_NEAR(math::Remap(0.25f, 0.0f, 1.0f, 1.0f, 0.0f), 0.75f, testkit::kTolerance);
}

/// @name 符号と丸め

TEST_F(MathUtilsTest, SignReturnsZeroForZero)
{
    /// @note ±1 の 2 値ではない。方向の «無し» を呼び出し側で分岐できるようにするため。
    EXPECT_NEAR(math::Sign(0.0f), 0.0f, testkit::kTolerance);
    EXPECT_NEAR(math::Sign(3.0f), 1.0f, testkit::kTolerance);
    EXPECT_NEAR(math::Sign(-3.0f), -1.0f, testkit::kTolerance);
}

TEST_F(MathUtilsTest, AbsMatchesTheSignedMagnitude)
{
    for (int i = 0; i < 64; ++i) {
        const float v = Rng().NextFloat(-100.0f, 100.0f);

        EXPECT_NEAR(math::Abs(v), v * math::Sign(v), testkit::kTolerance);
    }
}

TEST_F(MathUtilsTest, RoundingHelpersFollowTheStandardDirections)
{
    EXPECT_NEAR(math::Floor(-1.5f), -2.0f, testkit::kTolerance);
    EXPECT_NEAR(math::Ceil(-1.5f), -1.0f, testkit::kTolerance);
    /// @note 0 から遠い側へ
    EXPECT_NEAR(math::Round(-1.5f), -2.0f, testkit::kTolerance);
    EXPECT_NEAR(math::Round(1.5f), 2.0f, testkit::kTolerance);
}

TEST_F(MathUtilsTest, MinAndMaxSelectTheExpectedOperand)
{
    EXPECT_NEAR(math::Min(-3.0f, 2.0f), -3.0f, testkit::kTolerance);
    EXPECT_NEAR(math::Max(-3.0f, 2.0f), 2.0f, testkit::kTolerance);
}

TEST_F(MathUtilsTest, SqrtAndPowAgreeOnTheHalfExponent)
{
    EXPECT_NEAR(math::Sqrt(9.0f), math::Pow(9.0f, 0.5f), testkit::kTolerance);
}

/// @name 近似比較

TEST_F(MathUtilsTest, NearlyEqualAcceptsDifferencesBelowEpsilon)
{
    EXPECT_TRUE(math::NearlyEqual(1.0f, 1.0f + math::EPSILON * 0.5f));
    EXPECT_FALSE(math::NearlyEqual(1.0f, 1.0f + math::EPSILON * 10.0f));
}

TEST_F(MathUtilsTest, NearlyEqualHonoursAnExplicitEpsilon)
{
    EXPECT_TRUE(math::NearlyEqual(1.0f, 1.05f, 0.1f));
    EXPECT_FALSE(math::NearlyEqual(1.0f, 1.05f, 0.01f));
}

TEST_F(MathUtilsTest, NearlyZeroIsSymmetricAroundZero)
{
    EXPECT_TRUE(math::NearlyZero(math::EPSILON * 0.5f));
    EXPECT_TRUE(math::NearlyZero(-math::EPSILON * 0.5f));
    EXPECT_FALSE(math::NearlyZero(math::EPSILON * 10.0f));
}

} // namespace fbzz::tests
