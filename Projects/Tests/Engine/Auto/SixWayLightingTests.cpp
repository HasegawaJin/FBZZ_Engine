/// @file    SixWayLightingTests.cpp
/// @brief   6 方向ライトマップの重み (焼く側と読む側の共通規約) を固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// 焼く側 (VolumeRaymarch.hlsl) も読む側 (ParticleCommon.hlsli) も HLSL で、並びや符号が
/// ずれても何のエラーも出ない。ここでは C++ の写し (SixWayLighting.hpp) で規約を固定する。

#include <TestKit/TestKit.hpp>

#include <Engine/Asset/SixWayLighting.hpp>

#include <cmath>

namespace fbzz::tests {
namespace {

float WeightSum(const asset::SixWayWeights& w)
{
    return w.positive.x + w.positive.y + w.positive.z + w.negative.x + w.negative.y + w.negative.z;
}

} // namespace

TEST(SixWayLightingTest, WeightsOfAUnitDirectionSumToOne)
{
    const math::Vector3 directions[] = {
        { 1.0f, 0.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, { 0.6f, 0.8f, 0.0f },
        { -0.48f, 0.6f, -0.64f }, { 0.57735f, -0.57735f, 0.57735f },
    };
    for (const math::Vector3& direction : directions)
        EXPECT_NEAR(WeightSum(asset::ComputeSixWayWeights(direction)), 1.0f, 1.0e-4f);
}

TEST(SixWayLightingTest, AxisAlignedLightReadsExactlyOneChannel)
{
    // 並びは Positive = (右, 上, 奥) / Negative = (左, 下, 手前)。
    const math::Vector3 positiveMap{ 0.1f, 0.2f, 0.3f };
    const math::Vector3 negativeMap{ 0.4f, 0.5f, 0.6f };
    EXPECT_NEAR(asset::EvaluateSixWay(positiveMap, negativeMap, { 1.0f, 0.0f, 0.0f }), 0.1f, 1.0e-6f);
    EXPECT_NEAR(asset::EvaluateSixWay(positiveMap, negativeMap, { 0.0f, 1.0f, 0.0f }), 0.2f, 1.0e-6f);
    EXPECT_NEAR(asset::EvaluateSixWay(positiveMap, negativeMap, { 0.0f, 0.0f, 1.0f }), 0.3f, 1.0e-6f);
    EXPECT_NEAR(asset::EvaluateSixWay(positiveMap, negativeMap, { -1.0f, 0.0f, 0.0f }), 0.4f, 1.0e-6f);
    EXPECT_NEAR(asset::EvaluateSixWay(positiveMap, negativeMap, { 0.0f, -1.0f, 0.0f }), 0.5f, 1.0e-6f);
    EXPECT_NEAR(asset::EvaluateSixWay(positiveMap, negativeMap, { 0.0f, 0.0f, -1.0f }), 0.6f, 1.0e-6f);
}

TEST(SixWayLightingTest, DiagonalLightBlendsTheTwoFacingChannels)
{
    // 右上 45 度から来る光は «右» と «上» を半分ずつ読む。反対側 (左・下) は読まない。
    const float h = std::sqrt(0.5f);
    const asset::SixWayWeights w = asset::ComputeSixWayWeights({ h, h, 0.0f });
    EXPECT_NEAR(w.positive.x, 0.5f, 1.0e-5f);
    EXPECT_NEAR(w.positive.y, 0.5f, 1.0e-5f);
    EXPECT_NEAR(w.negative.x, 0.0f, 1.0e-6f);
    EXPECT_NEAR(w.negative.y, 0.0f, 1.0e-6f);
}

TEST(SixWayLightingTest, UniformMapsRespondTheSameFromEveryDirection)
{
    // 全方向が同じ明るさなら、光の向きを回しても明るさは変わらない (重みの和が 1 だから)。
    const math::Vector3 uniform{ 0.7f, 0.7f, 0.7f };
    EXPECT_NEAR(asset::EvaluateSixWay(uniform, uniform, { 0.6f, -0.8f, 0.0f }), 0.7f, 1.0e-5f);
    EXPECT_NEAR(asset::EvaluateSixWay(uniform, uniform, { 0.0f, 0.6f, -0.8f }), 0.7f, 1.0e-5f);
    EXPECT_NEAR(asset::EvaluateSixWayAmbient(uniform, uniform), 0.7f, 1.0e-6f);
}

} // namespace fbzz::tests
