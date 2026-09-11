/// @file    VolumeFlipbookAnalyticTests.cpp
/// @brief   解析ボリュームの速度が «密度が実際に動いた量» と一致することを固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// Volume Flipbook Baker の MV はこの速度をそのまま画面へ投影したもの。
/// 速度と密度の動きが食い違っていれば、MV は一見それらしいまま中身だけ間違う。
/// GPU 版 (VolumeFill.cs.hlsl) はこの CPU 版の写しなので、ここで式を押さえる。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Asset/VolumeFlipbookAnalytic.hpp>

#include <cmath>
#include <vector>

namespace fbzz::tests {
namespace {

class VolumeFlipbookAnalyticTest : public testkit::EngineFixture {};

constexpr float kFrameDt = 1.0f / 24.0f;

asset::VolumeSourceSettings PuffSource()
{
    asset::VolumeSourceSettings source;
    source.preset = asset::VolumeFlipbookPreset::Puff;
    source.seed = 3;
    // Puff はベイクの長さで正規化される。検査する時刻 (〜1.3 秒) がベイクの内側に入る長さにする。
    source.frameCount = 64;
    source.frameDt = kFrameDt;
    return source;
}

asset::VolumeFillConstants Pack(const std::vector<asset::VolumePuff>& puffs, float time)
{
    asset::VolumeFillConstants constants;
    asset::PackVolumeFillConstants(puffs, asset::VolumeNoiseSettings{}, 64, time, kFrameDt, constants);
    return constants;
}

// 単体 puff の中心付近から、密度のある点をいくつか拾う。
std::vector<math::Vector3> DensePoints(const asset::VolumeFillConstants& constants)
{
    std::vector<math::Vector3> points;
    for (float x = -0.3f; x <= 0.3f; x += 0.1f)
        for (float y = -0.6f; y <= 0.0f; y += 0.1f)
            for (float z = -0.2f; z <= 0.2f; z += 0.2f)
                if (asset::SampleVolumeFill(constants, { x, y, z }).density > 0.05f)
                    points.push_back({ x, y, z });
    return points;
}

} // namespace

TEST_F(VolumeFlipbookAnalyticTest, DensityIsCarriedExactlyBySecantVelocity)
{
    const std::vector<asset::VolumePuff> puffs = asset::BuildVolumePuffs(PuffSource());
    ASSERT_EQ(puffs.size(), 1u);
    constexpr float kTime = 0.5f;
    const asset::VolumeFillConstants now = Pack(puffs, kTime);
    const asset::VolumeFillConstants next = Pack(puffs, kTime + kFrameDt);

    const std::vector<math::Vector3> points = DensePoints(now);

    ASSERT_GE(points.size(), 5u);
    for (const math::Vector3& x : points) {
        const asset::VolumeSample here = asset::SampleVolumeFill(now, x);
        const math::Vector3 moved = x + here.velocity * kFrameDt;
        EXPECT_NEAR(asset::SampleVolumeFill(next, moved).density, here.density, testkit::kLooseTolerance)
            << "x=(" << x.x << "," << x.y << "," << x.z << ")";
    }
}

TEST_F(VolumeFlipbookAnalyticTest, PackedVelocityEqualsTheFlowMapOverOneFrame)
{
    const std::vector<asset::VolumePuff> puffs = asset::BuildVolumePuffs(PuffSource());
    ASSERT_EQ(puffs.size(), 1u);
    constexpr float kTime = 1.25f;
    const asset::VolumeFillConstants constants = Pack(puffs, kTime);

    for (const math::Vector3& x : DensePoints(constants)) {
        const math::Vector3 expected = (asset::PuffFlowMap(puffs[0], x, kTime, kTime + kFrameDt) - x) / kFrameDt;

        EXPECT_VEC3_NEAR(asset::SampleVolumeFill(constants, x).velocity, expected, testkit::kLooseTolerance);
    }
}

TEST_F(VolumeFlipbookAnalyticTest, SameSeedIsDeterministicAndOtherSeedsDiffer)
{
    asset::VolumeSourceSettings source;
    source.preset = asset::VolumeFlipbookPreset::RisingPlume;
    source.seed = 7;
    asset::VolumeSourceSettings other = source;
    other.seed = 8;
    const float time = asset::ResolveVolumeStartTime(source);

    const asset::VolumeFillConstants a = Pack(asset::BuildVolumePuffs(source), time);
    const asset::VolumeFillConstants b = Pack(asset::BuildVolumePuffs(source), time);
    const asset::VolumeFillConstants c = Pack(asset::BuildVolumePuffs(other), time);

    float differenceToOtherSeed = 0.0f;
    for (float y = -0.8f; y <= 0.8f; y += 0.1f) {
        const math::Vector3 x = { 0.02f, y, -0.03f };
        EXPECT_EQ(asset::SampleVolumeFill(a, x).density, asset::SampleVolumeFill(b, x).density);
        differenceToOtherSeed += std::fabs(asset::SampleVolumeFill(a, x).density
                                           - asset::SampleVolumeFill(c, x).density);
    }
    EXPECT_GT(differenceToOtherSeed, 0.01f);
}

TEST_F(VolumeFlipbookAnalyticTest, LoopingPlumeReturnsToTheFirstFrameAfterItsDuration)
{
    asset::VolumeSourceSettings source;
    source.preset = asset::VolumeFlipbookPreset::RisingPlume;
    source.seed = 5;
    source.loop = true;
    source.frameCount = 48;
    source.frameDt = kFrameDt;
    const std::vector<asset::VolumePuff> puffs = asset::BuildVolumePuffs(source);
    const float start = asset::ResolveVolumeStartTime(source);
    const float duration = static_cast<float>(source.frameCount) * source.frameDt;

    const asset::VolumeFillConstants first = Pack(puffs, start);
    const asset::VolumeFillConstants wrapped = Pack(puffs, start + duration);

    for (float y = -0.8f; y <= 0.8f; y += 0.1f) {
        for (float x = -0.3f; x <= 0.3f; x += 0.15f) {
            const math::Vector3 p = { x, y, 0.05f };
            EXPECT_NEAR(asset::SampleVolumeFill(wrapped, p).density,
                        asset::SampleVolumeFill(first, p).density, testkit::kLooseTolerance)
                << "p=(" << x << "," << y << ")";
        }
    }
}

TEST_F(VolumeFlipbookAnalyticTest, HotCoreCoolsAsThePuffAges)
{
    asset::VolumePuff puff;
    puff.startCenter = { 0.0f, -0.5f, 0.0f };
    puff.velocity = { 0.0f, 0.4f, 0.0f };
    puff.radius = 0.3f;
    puff.temperature = 1.0f;
    puff.coolingTime = 0.5f;
    const std::vector<asset::VolumePuff> puffs = { puff };

    const float young = asset::SampleVolumeFill(Pack(puffs, 0.1f), { 0.0f, -0.46f, 0.0f }).temperature;
    const float old = asset::SampleVolumeFill(Pack(puffs, 1.0f), { 0.0f, -0.1f, 0.0f }).temperature;

    EXPECT_GT(young, 0.0f);
    EXPECT_LT(old, young * 0.5f);
}

TEST_F(VolumeFlipbookAnalyticTest, PlumeNeverNeedsMorePuffsThanTheGpuHolds)
{
    asset::VolumeSourceSettings source;
    source.preset = asset::VolumeFlipbookPreset::RisingPlume;
    source.frameCount = 64;
    source.frameDt = kFrameDt;
    const std::vector<asset::VolumePuff> puffs = asset::BuildVolumePuffs(source);
    const float start = asset::ResolveVolumeStartTime(source);

    for (int frame = 0; frame < source.frameCount; ++frame) {
        asset::VolumeFillConstants constants;
        std::uint32_t alive = 0;
        for (const asset::VolumePuff& puff : puffs) {
            const float age = start + static_cast<float>(frame) * kFrameDt - puff.birthTime;
            if (age >= 0.0f && age <= puff.lifetime) ++alive;
        }

        const std::uint32_t packed = asset::PackVolumeFillConstants(
            puffs, asset::VolumeNoiseSettings{}, 64, start + static_cast<float>(frame) * kFrameDt, kFrameDt, constants);

        EXPECT_GT(packed, 0u) << "frame=" << frame;
        EXPECT_LE(alive, asset::kVolumeFillMaxPuffs) << "frame=" << frame;
    }
}

TEST_F(VolumeFlipbookAnalyticTest, DensityVanishesOutsideTheCullRadius)
{
    asset::VolumeNoiseSettings noise;
    noise.amplitude = 1.0f;
    const math::Vector3 directions[] = {
        { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f },
        { 0.577f, 0.577f, 0.577f }, { -0.707f, 0.0f, 0.707f }, { 0.0f, -0.6f, 0.8f } };

    for (const float seed : { 0.0f, 13.0f, 71.5f }) {
        for (const math::Vector3& direction : directions) {
            const math::Vector3 y = direction * (asset::kVolumePuffBodyCullRadius + 0.001f);
            EXPECT_EQ(asset::EvaluatePuffBodyDensity(y, seed, noise), 0.0f) << "seed=" << seed;
        }
    }
}

} // namespace fbzz::tests
