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
#include <Engine/Asset/VolumeFlipbookSources.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {

class VolumeFlipbookAnalyticTest : public testkit::EngineFixture {};

constexpr float kFrameDt = 1.0f / 24.0f;

asset::VolumeSourceSettings PuffSource()
{
    asset::VolumeSourceSettings source;
    source.preset = "Puff";
    source.seed = 3;
    /// @note Puff はベイクの長さで正規化される。検査する時刻 (〜1.3 秒) がベイクの内側に入る長さにする。
    source.frameCount = 64;
    source.frameDt = kFrameDt;
    return source;
}

asset::VolumeFillFrame Pack(const std::vector<asset::VolumePuff>& puffs, float time)
{
    asset::VolumeFillFrame frame;
    asset::PackVolumeFill(puffs, asset::VolumeNoiseSettings{}, 64, time, kFrameDt, frame);
    return frame;
}

/// center の周りから、密度のある点をいくつか拾う。
std::vector<math::Vector3> DensePointsAround(const asset::VolumeFillFrame& frame, const math::Vector3& center,
                                             float extent)
{
    std::vector<math::Vector3> points;
    const float step = extent / 3.0f;
    for (float x = -extent; x <= extent; x += step)
        for (float y = -extent; y <= extent; y += step)
            for (float z = -extent; z <= extent; z += extent)
                if (const math::Vector3 p = center + math::Vector3{ x, y, z };
                    asset::SampleVolumeFill(frame, p).density > 0.05f)
                    points.push_back(p);
    return points;
}

std::vector<math::Vector3> DensePoints(const asset::VolumeFillFrame& frame)
{
    return DensePointsAround(frame, { 0.0f, -0.3f, 0.0f }, 0.3f);
}

/// 重力・減速・進行方向への伸び・回転・膨張を全部持つ puff。
asset::VolumePuff BallisticPuff()
{
    asset::VolumePuff puff;
    puff.startCenter = { -0.3f, -0.2f, 0.0f };
    puff.velocity = { 0.8f, 1.2f, 0.1f };
    puff.acceleration = { 0.0f, -2.5f, 0.0f };
    puff.drag = 0.6f;
    puff.angularVelocity = { 0.5f, 1.5f, 0.2f };
    puff.expansionRate = 0.3f;
    puff.radius = 0.25f;
    puff.stretch = 1.2f;
    puff.stretchPerSpeed = 0.6f;
    puff.density = 1.5f;
    return puff;
}

} // namespace

TEST_F(VolumeFlipbookAnalyticTest, DensityIsCarriedExactlyBySecantVelocity)
{
    const std::vector<asset::VolumePuff> puffs = asset::BuildVolumePuffs(PuffSource());
    ASSERT_EQ(puffs.size(), 1u);
    constexpr float kTime = 0.5f;
    const asset::VolumeFillFrame now = Pack(puffs, kTime);
    const asset::VolumeFillFrame next = Pack(puffs, kTime + kFrameDt);

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
    const asset::VolumeFillFrame frame = Pack(puffs, kTime);

    for (const math::Vector3& x : DensePoints(frame)) {
        const math::Vector3 expected = (asset::PuffFlowMap(puffs[0], x, kTime, kTime + kFrameDt) - x) / kFrameDt;

        EXPECT_VEC3_NEAR(asset::SampleVolumeFill(frame, x).velocity, expected, testkit::kLooseTolerance);
    }
}

TEST_F(VolumeFlipbookAnalyticTest, GravityDragAndStretchStillCarryDensityExactly)
{
    const std::vector<asset::VolumePuff> puffs = { BallisticPuff() };
    constexpr float kTime = 0.4f;
    const asset::VolumeFillFrame now = Pack(puffs, kTime);
    const asset::VolumeFillFrame next = Pack(puffs, kTime + kFrameDt);

    const std::vector<math::Vector3> points = DensePointsAround(now, asset::PuffCenterAt(puffs[0], kTime), 0.2f);

    ASSERT_GE(points.size(), 5u);
    for (const math::Vector3& x : points) {
        const asset::VolumeSample here = asset::SampleVolumeFill(now, x);
        const math::Vector3 flow = (asset::PuffFlowMap(puffs[0], x, kTime, kTime + kFrameDt) - x) / kFrameDt;
        EXPECT_VEC3_NEAR(here.velocity, flow, testkit::kLooseTolerance);
        EXPECT_NEAR(asset::SampleVolumeFill(next, x + here.velocity * kFrameDt).density, here.density,
                    testkit::kLooseTolerance)
            << "x=(" << x.x << "," << x.y << "," << x.z << ")";
    }
}

TEST_F(VolumeFlipbookAnalyticTest, CenterVelocityIsTheDerivativeOfTheCenter)
{
    asset::VolumePuff withDrag = BallisticPuff();
    asset::VolumePuff withoutDrag = BallisticPuff();
    withoutDrag.drag = 0.0f;
    constexpr float kH = 1.0e-3f;

    for (const float age : { 0.1f, 0.5f, 1.2f }) {
        for (const asset::VolumePuff& puff : { withDrag, withoutDrag }) {
            const math::Vector3 numeric =
                (asset::PuffCenterAt(puff, age + kH) - asset::PuffCenterAt(puff, age - kH)) / (2.0f * kH);
            EXPECT_VEC3_NEAR(asset::PuffCenterVelocityAt(puff, age), numeric, testkit::kLooseTolerance);
        }
    }
    /// @note 減速なしは放物線そのもの。
    const math::Vector3 parabola = withoutDrag.startCenter + withoutDrag.velocity * 0.5f
        + withoutDrag.acceleration * (0.5f * 0.25f);
    EXPECT_VEC3_NEAR(asset::PuffCenterAt(withoutDrag, 0.5f), parabola, testkit::kTolerance);
}

TEST_F(VolumeFlipbookAnalyticTest, StretchElongatesAlongTheVelocity)
{
    asset::VolumePuff puff;
    puff.velocity = { 1.0f, 0.0f, 0.0f };
    puff.radius = 0.2f;
    puff.stretch = 3.0f;
    puff.noiseScale = 0.0f;
    const asset::VolumeFillFrame frame = Pack({ puff }, 0.0f);

    /// @note 進行方向は σ·r = 0.6 まで伸び、横は r/√σ ≈ 0.115 まで縮む。
    EXPECT_GT(asset::SampleVolumeFill(frame, { 0.4f, 0.0f, 0.0f }).density, 0.0f);
    EXPECT_EQ(asset::SampleVolumeFill(frame, { 0.0f, 0.4f, 0.0f }).density, 0.0f);
    EXPECT_EQ(asset::SampleVolumeFill(frame, { 0.0f, 0.0f, 0.4f }).density, 0.0f);
}

TEST_F(VolumeFlipbookAnalyticTest, ColorKeyAndLiquidAreCarriedToTheMedium)
{
    asset::VolumePuff smoke;
    smoke.startCenter = { -0.5f, 0.0f, 0.0f };
    smoke.radius = 0.2f;
    smoke.colorKey = 0.25f;
    asset::VolumePuff water = smoke;
    water.startCenter = { 0.5f, 0.0f, 0.0f };
    water.colorKey = 0.75f;
    water.liquid = 1.0f;
    const asset::VolumeFillFrame frame = Pack({ smoke, water }, 0.0f);

    const asset::VolumeSample left = asset::SampleVolumeFill(frame, { -0.5f, 0.0f, 0.0f });
    const asset::VolumeSample right = asset::SampleVolumeFill(frame, { 0.5f, 0.0f, 0.0f });

    EXPECT_NEAR(left.colorKey, 0.25f, testkit::kTolerance);
    EXPECT_NEAR(left.liquid, 0.0f, testkit::kTolerance);
    EXPECT_NEAR(right.colorKey, 0.75f, testkit::kTolerance);
    EXPECT_NEAR(right.liquid, 1.0f, testkit::kTolerance);
}

TEST_F(VolumeFlipbookAnalyticTest, SameSeedIsDeterministicAndOtherSeedsDiffer)
{
    asset::VolumeSourceSettings source;
    source.preset = "RisingPlume";
    source.seed = 7;
    asset::VolumeSourceSettings other = source;
    other.seed = 8;
    const float time = asset::ResolveVolumeStartTime(source);

    const asset::VolumeFillFrame a = Pack(asset::BuildVolumePuffs(source), time);
    const asset::VolumeFillFrame b = Pack(asset::BuildVolumePuffs(source), time);
    const asset::VolumeFillFrame c = Pack(asset::BuildVolumePuffs(other), time);

    float differenceToOtherSeed = 0.0f;
    for (float y = -0.8f; y <= 0.8f; y += 0.1f) {
        const math::Vector3 x = { 0.02f, y, -0.03f };
        EXPECT_EQ(asset::SampleVolumeFill(a, x).density, asset::SampleVolumeFill(b, x).density);
        differenceToOtherSeed += std::fabs(asset::SampleVolumeFill(a, x).density
                                           - asset::SampleVolumeFill(c, x).density);
    }
    EXPECT_GT(differenceToOtherSeed, 0.01f);
}

TEST_F(VolumeFlipbookAnalyticTest, LoopingSourcesReturnToTheFirstFrameAfterTheirDuration)
{
    std::vector<asset::VolumeSourceSettings> sources;
    for (const char* name : { "RisingPlume", "Torch", "Fountain" }) {
        asset::VolumeSourceSettings source;
        source.preset = name;
        sources.push_back(source);
    }
    asset::VolumeSourceSettings emitter;
    emitter.preset = asset::kVolumeEmitterSourceName;
    emitter.emitter.burst = false;
    emitter.emitter.count = 30;
    sources.push_back(emitter);

    for (asset::VolumeSourceSettings& source : sources) {
        source.seed = 5;
        source.loop = true;
        source.frameCount = 48;
        source.frameDt = kFrameDt;
        ASSERT_TRUE(asset::VolumeSourceLoops(source)) << source.preset;
        const std::vector<asset::VolumePuff> puffs = asset::BuildVolumePuffs(source);
        const float start = asset::ResolveVolumeStartTime(source);
        const float duration = static_cast<float>(source.frameCount) * source.frameDt;

        const asset::VolumeFillFrame first = Pack(puffs, start);
        const asset::VolumeFillFrame wrapped = Pack(puffs, start + duration);

        float total = 0.0f;
        for (float y = -0.8f; y <= 0.8f; y += 0.1f) {
            for (float x = -0.3f; x <= 0.3f; x += 0.15f) {
                const math::Vector3 p = { x, y, 0.05f };
                const float expected = asset::SampleVolumeFill(first, p).density;
                total += expected;
                EXPECT_NEAR(asset::SampleVolumeFill(wrapped, p).density, expected, testkit::kLooseTolerance)
                    << source.preset << " p=(" << x << "," << y << ")";
            }
        }
        EXPECT_GT(total, 0.1f) << source.preset << " の検査点に何も写っていない";
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

TEST_F(VolumeFlipbookAnalyticTest, PlumeAlwaysHasPuffsToDraw)
{
    asset::VolumeSourceSettings source;
    source.preset = "RisingPlume";
    source.frameCount = 64;
    source.frameDt = kFrameDt;
    const std::vector<asset::VolumePuff> puffs = asset::BuildVolumePuffs(source);
    const float start = asset::ResolveVolumeStartTime(source);

    for (int frame = 0; frame < source.frameCount; ++frame) {
        asset::VolumeFillFrame packed;
        EXPECT_GT(asset::PackVolumeFill(puffs, asset::VolumeNoiseSettings{}, 64,
                                        start + static_cast<float>(frame) * kFrameDt, kFrameDt, packed),
                  0u)
            << "frame=" << frame;
    }
}

TEST_F(VolumeFlipbookAnalyticTest, EveryBuiltinSourceFitsInThePuffBuffer)
{
    for (const asset::VolumeSourceDesc& desc : asset::VolumeSources()) {
        asset::VolumeSourceSettings source;
        source.preset = desc.name;
        source.frameCount = 64;
        source.frameDt = kFrameDt;
        const std::vector<asset::VolumePuff> puffs = asset::BuildVolumePuffs(source);
        const float start = asset::ResolveVolumeStartTime(source);
        EXPECT_FALSE(puffs.empty()) << desc.name;

        for (int frame = 0; frame < source.frameCount; ++frame) {
            const float time = start + static_cast<float>(frame) * kFrameDt;
            EXPECT_LE(asset::CountLiveVolumePuffs(puffs, time), asset::kVolumeFillMaxPuffs)
                << desc.name << " frame=" << frame;
        }
    }
}

TEST_F(VolumeFlipbookAnalyticTest, PackDropsPuffsBeyondTheBufferCapacity)
{
    std::vector<asset::VolumePuff> puffs(asset::kVolumeFillMaxPuffs + 10);

    asset::VolumeFillFrame frame;
    const std::uint32_t packed = asset::PackVolumeFill(puffs, asset::VolumeNoiseSettings{}, 64, 0.0f, kFrameDt, frame);

    EXPECT_EQ(packed, asset::kVolumeFillMaxPuffs);
    EXPECT_EQ(frame.puffs.size(), static_cast<std::size_t>(asset::kVolumeFillMaxPuffs));
    EXPECT_EQ(frame.header.puffCount, asset::kVolumeFillMaxPuffs);
}

TEST_F(VolumeFlipbookAnalyticTest, RegisteredSourceIsBuiltAndReplacedByName)
{
    const auto makeSource = [](std::size_t count) {
        asset::VolumeSourceDesc desc;
        desc.name = "Test.Custom";
        desc.build = [count](const asset::VolumeSourceSettings&, const asset::VolumeSourceRange& range) {
            std::vector<asset::VolumePuff> puffs(count);
            for (asset::VolumePuff& puff : puffs) puff.birthTime = range.start;
            return puffs;
        };
        return desc;
    };
    asset::VolumeSourceSettings source;
    source.preset = "Test.Custom";

    asset::RegisterVolumeSource(makeSource(2));
    EXPECT_EQ(asset::BuildVolumePuffs(source).size(), 2u);
    asset::RegisterVolumeSource(makeSource(3));
    EXPECT_EQ(asset::BuildVolumePuffs(source).size(), 3u);

    int sameName = 0;
    for (const asset::VolumeSourceDesc& desc : asset::VolumeSources())
        if (desc.name == "Test.Custom") ++sameName;
    EXPECT_EQ(sameName, 1);
}

TEST_F(VolumeFlipbookAnalyticTest, UnknownSourceBuildsNothing)
{
    asset::VolumeSourceSettings source;
    source.preset = "NoSuchSource";

    EXPECT_TRUE(asset::BuildVolumePuffs(source).empty());
    EXPECT_FALSE(asset::VolumeSourceCanLoop(source));
}

TEST_F(VolumeFlipbookAnalyticTest, BuiltinSourcesAreRegistered)
{
    for (const char* name : { "Puff", "RisingPlume", "Fireball", "Torch", "Fountain", "WaterSplash",
                              "BloodSpray", "BloodBurst", asset::kVolumeEmitterSourceName }) {
        EXPECT_NE(asset::FindVolumeSource(name), nullptr) << name;
    }
}

TEST_F(VolumeFlipbookAnalyticTest, EmitterBurstSpawnsCountPuffsAtTheStart)
{
    asset::VolumeSourceSettings source;
    source.preset = asset::kVolumeEmitterSourceName;
    source.emitter.burst = true;
    source.emitter.count = 10;

    const std::vector<asset::VolumePuff> puffs = asset::BuildVolumePuffs(source);

    ASSERT_EQ(puffs.size(), 10u);
    for (const asset::VolumePuff& puff : puffs) EXPECT_EQ(puff.birthTime, asset::ResolveVolumeStartTime(source));
    EXPECT_FALSE(asset::VolumeSourceCanLoop(source));
}

TEST_F(VolumeFlipbookAnalyticTest, RandomConeStaysInsideItsAngle)
{
    const math::Vector3 axis = math::Vector3{ 1.0f, 0.35f, 0.0f }.NormalizedOr(math::Vector3::RIGHT);
    const float halfAngle = 0.35f;
    for (std::uint32_t i = 0; i < 64; ++i) {
        const math::Vector3 direction = asset::VolumeRandomInCone(axis, halfAngle, 9, i, 0);
        EXPECT_NEAR(direction.Length(), 1.0f, testkit::kLooseTolerance);
        EXPECT_GE(math::Vector3::Dot(direction, axis), std::cos(halfAngle) - testkit::kLooseTolerance);
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
