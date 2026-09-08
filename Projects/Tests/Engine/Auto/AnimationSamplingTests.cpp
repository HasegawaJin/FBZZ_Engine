/// @file    AnimationSamplingTests.cpp
/// @brief   キー列から 1 つの値を取り出す規則 (端点・補間・範囲外・キー 1 本) を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// .anim (AnimatorSystem) と .sequence (SequenceSystem) が同じ関数を共有している。
/// 補間が狂うと «アニメが 1 フレームだけ飛ぶ» «演出とキャラの動きが微妙にずれる» の
/// どちらかになり、原因を目で追えない。端点と中間を数値として固定する。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Asset/AnimationSampling.hpp>

#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>

#include <cmath>
#include <vector>

namespace fbzz::tests {
namespace {

std::vector<asset::VectorKey> VectorRamp()
{
    return { { 0.0, { 0.0f, 0.0f, 0.0f } }, { 10.0, { 10.0f, 20.0f, -30.0f } } };
}

std::vector<asset::FloatKey> FloatRamp()
{
    asset::FloatKey a;
    a.time  = 0.0;
    a.value = 0.0f;
    asset::FloatKey b;
    b.time  = 10.0;
    b.value = 100.0f;
    return { a, b };
}

} // namespace

class AnimationSamplingTest : public testkit::EngineFixture {};

// --- 空・1 本 ---------------------------------------------------------------

TEST_F(AnimationSamplingTest, EmptyKeysFallBackToTheGivenValue)
{
    // キーが無いチャンネルは «触らない» が正。0 を返すとボーンが原点へ吸われる。
    const math::Vector3 fallback{ 1.0f, 2.0f, 3.0f };

    EXPECT_VEC3_NEAR(asset::SampleVectorKeys({}, 5.0, fallback), fallback,
                     testkit::kTolerance);
    EXPECT_QUAT_NEAR(asset::SampleQuaternionKeys({}, 5.0,
                                                 math::Quaternion::Identity()),
                     math::Quaternion::Identity(), testkit::kTolerance);
}

TEST_F(AnimationSamplingTest, ASingleKeyHoldsItsValueForever)
{
    const std::vector<asset::VectorKey> single{ { 5.0, { 7.0f, 8.0f, 9.0f } } };

    EXPECT_VEC3_NEAR(asset::SampleVectorKeys(single, -100.0, math::Vector3::ZERO),
                     math::Vector3(7.0f, 8.0f, 9.0f), testkit::kTolerance);
    EXPECT_VEC3_NEAR(asset::SampleVectorKeys(single, 100.0, math::Vector3::ZERO),
                     math::Vector3(7.0f, 8.0f, 9.0f), testkit::kTolerance);
}

// --- 端点と範囲外 -----------------------------------------------------------

TEST_F(AnimationSamplingTest, ReturnsTheEndpointsExactly)
{
    const auto keys = VectorRamp();

    EXPECT_VEC3_NEAR(asset::SampleVectorKeys(keys, 0.0, math::Vector3::ZERO),
                     math::Vector3::ZERO, testkit::kTolerance);
    EXPECT_VEC3_NEAR(asset::SampleVectorKeys(keys, 10.0, math::Vector3::ZERO),
                     math::Vector3(10.0f, 20.0f, -30.0f), testkit::kTolerance);
}

TEST_F(AnimationSamplingTest, ClampsOutsideTheKeyRange)
{
    // 尺の外を «ループの先頭» や «外挿» にしない。繰り返しは呼び出し側の仕事。
    const auto keys = VectorRamp();

    EXPECT_VEC3_NEAR(asset::SampleVectorKeys(keys, -50.0, math::Vector3::ZERO),
                     math::Vector3::ZERO, testkit::kTolerance);
    EXPECT_VEC3_NEAR(asset::SampleVectorKeys(keys, 50.0, math::Vector3::ZERO),
                     math::Vector3(10.0f, 20.0f, -30.0f), testkit::kTolerance);
}

// --- 補間 -------------------------------------------------------------------

TEST_F(AnimationSamplingTest, InterpolatesLinearlyBetweenTwoKeys)
{
    const auto keys = VectorRamp();

    EXPECT_VEC3_NEAR(asset::SampleVectorKeys(keys, 5.0, math::Vector3::ZERO),
                     math::Vector3(5.0f, 10.0f, -15.0f), testkit::kTolerance);
    EXPECT_VEC3_NEAR(asset::SampleVectorKeys(keys, 2.5, math::Vector3::ZERO),
                     math::Vector3(2.5f, 5.0f, -7.5f), testkit::kTolerance);
}

TEST_F(AnimationSamplingTest, StepInterpolationHoldsThePreviousKey)
{
    // 表情の切り替えやフラグは «途中の値» を作ってはいけない。
    const auto keys = VectorRamp();

    EXPECT_VEC3_NEAR(
        asset::SampleVectorKeys(keys, 9.99, math::Vector3::ZERO, asset::AnimInterp::Step),
        math::Vector3::ZERO, testkit::kTolerance);
}

TEST_F(AnimationSamplingTest, PicksTheRightSegmentAmongManyKeys)
{
    // upper_bound の境界。1 つずれると «常に前のキーの値» になる。
    const std::vector<asset::VectorKey> keys{
        { 0.0, { 0.0f, 0.0f, 0.0f } },
        { 1.0, { 10.0f, 0.0f, 0.0f } },
        { 2.0, { 20.0f, 0.0f, 0.0f } },
        { 3.0, { 30.0f, 0.0f, 0.0f } },
    };

    EXPECT_NEAR(asset::SampleVectorKeys(keys, 1.5, math::Vector3::ZERO).x, 15.0f,
                testkit::kTolerance);
    EXPECT_NEAR(asset::SampleVectorKeys(keys, 2.0, math::Vector3::ZERO).x, 20.0f,
                testkit::kTolerance);
    EXPECT_NEAR(asset::SampleVectorKeys(keys, 2.5, math::Vector3::ZERO).x, 25.0f,
                testkit::kTolerance);
}

TEST_F(AnimationSamplingTest, SurvivesTwoKeysAtTheSameTime)
{
    // 書き出し側が同じ時刻へ 2 本置くことがある。0 除算で NaN を返さないこと。
    const std::vector<asset::VectorKey> keys{
        { 0.0, { 0.0f, 0.0f, 0.0f } },
        { 1.0, { 10.0f, 0.0f, 0.0f } },
        { 1.0, { 20.0f, 0.0f, 0.0f } },
        { 2.0, { 30.0f, 0.0f, 0.0f } },
    };

    const math::Vector3 sampled = asset::SampleVectorKeys(keys, 1.0, math::Vector3::ZERO);

    EXPECT_FALSE(std::isnan(sampled.x));
}

// --- クォータニオン ---------------------------------------------------------

TEST_F(AnimationSamplingTest, SlerpsRotationsAndKeepsThemNormalised)
{
    const std::vector<asset::QuaternionKey> keys{
        { 0.0, math::Quaternion::Identity() },
        { 10.0, math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(90.0f)) },
    };

    const math::Quaternion mid =
        asset::SampleQuaternionKeys(keys, 5.0, math::Quaternion::Identity());

    EXPECT_QUAT_NEAR(mid,
                     math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(45.0f)),
                     testkit::kLooseTolerance);
    EXPECT_NEAR(mid.Length(), 1.0f, testkit::kLooseTolerance);
}

TEST_F(AnimationSamplingTest, RotationStepHoldsThePreviousKey)
{
    const std::vector<asset::QuaternionKey> keys{
        { 0.0, math::Quaternion::Identity() },
        { 10.0, math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(90.0f)) },
    };

    EXPECT_QUAT_NEAR(asset::SampleQuaternionKeys(keys, 9.0, math::Quaternion::Identity(),
                                                 asset::AnimInterp::Step),
                     math::Quaternion::Identity(), testkit::kTolerance);
}

// --- スカラー ---------------------------------------------------------------

TEST_F(AnimationSamplingTest, SamplesFloatsLinearly)
{
    const auto keys = FloatRamp();

    EXPECT_NEAR(asset::SampleFloatKeys(keys, 5.0, asset::AnimInterp::Linear), 50.0f,
                testkit::kTolerance);
}

TEST_F(AnimationSamplingTest, EmptyFloatKeysGiveZero)
{
    EXPECT_NEAR(asset::SampleFloatKeys({}, 5.0, asset::AnimInterp::Linear), 0.0f,
                testkit::kTolerance);
}

TEST_F(AnimationSamplingTest, CubicMatchesLinearAtTheKeysThemselves)
{
    // 接線が何であれ、キー上の値はキーの値。ここがずれると «キーを打った瞬間だけ飛ぶ»。
    const auto keys = FloatRamp();

    EXPECT_NEAR(asset::SampleFloatKeys(keys, 0.0, asset::AnimInterp::Cubic), 0.0f,
                testkit::kLooseTolerance);
    EXPECT_NEAR(asset::SampleFloatKeys(keys, 10.0, asset::AnimInterp::Cubic), 100.0f,
                testkit::kLooseTolerance);
}

TEST_F(AnimationSamplingTest, CubicWithFlatTangentsStaysInsideTheRange)
{
    // 接線 0 の Hermite は行き過ぎない。オーバーシュートすると
    // «クリップに無いポーズ» が 1 フレームだけ出る。
    const auto keys = FloatRamp();

    for (int step = 0; step <= 20; ++step) {
        const double ticks = static_cast<double>(step) * 0.5;
        const float value = asset::SampleFloatKeys(keys, ticks, asset::AnimInterp::Cubic);

        EXPECT_GE(value, -testkit::kLooseTolerance) << "ticks=" << ticks;
        EXPECT_LE(value, 100.0f + testkit::kLooseTolerance) << "ticks=" << ticks;
    }
}

} // namespace fbzz::tests
