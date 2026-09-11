/// @file    VolumeFlipbookFramingTests.cpp
/// @brief   焼く前の構図検査 (箱やタイルの縁で煙が切れないか) の判定と、Look の既定値を固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// 既定のプリセットで警告が鳴ると、利用者は警告を読まなくなる。逆に鳴るべき設定で
/// 黙っていると、縁で切れた «四角い板» の煙を何十コマも焼いてしまう。両側を押さえる。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Asset/VolumeFlipbookBaker.hpp>

#include <cmath>
#include <string>

namespace fbzz::tests {
namespace {

class VolumeFlipbookFramingTest : public testkit::EngineFixture {};

asset::VolumeFlipbookBakeSettings SettingsFor(const std::string& preset, int frames)
{
    asset::VolumeFlipbookBakeSettings settings;
    settings.source.preset = preset;
    settings.source.frameCount = frames;
    return settings;
}

} // namespace

TEST_F(VolumeFlipbookFramingTest, DefaultPresetsStayInsideTheBoxAndTheTile)
{
    for (const asset::VolumeSourceDesc& desc : asset::VolumeSources()) {
        for (const int frames : { 32, 64, 128 }) {
            const asset::VolumeFramingReport report = asset::AnalyzeVolumeFraming(SettingsFor(desc.name, frames));

            ASSERT_EQ(report.frameIssues.size(), static_cast<std::size_t>(frames));
            EXPECT_EQ(report.boxCutFrames, 0) << "preset=" << desc.name << " frames=" << frames;
            EXPECT_EQ(report.tileCutFrames, 0) << "preset=" << desc.name << " frames=" << frames;
            EXPECT_EQ(report.overflowFrames, 0) << "preset=" << desc.name << " frames=" << frames;
        }
    }
}

TEST_F(VolumeFlipbookFramingTest, NarrowFramingIsReportedAsCutByTheTileEdge)
{
    asset::VolumeFlipbookBakeSettings settings = SettingsFor("Puff", 32);
    settings.halfExtent = 0.5f;

    const asset::VolumeFramingReport report = asset::AnalyzeVolumeFraming(settings);

    EXPECT_EQ(report.tileCutFrames, 32);
    EXPECT_EQ(report.boxCutFrames, 0);
    EXPECT_NE(report.frameIssues.front() & asset::kFramingCutByTileEdge, 0);
}

TEST_F(VolumeFlipbookFramingTest, TooManyLivePuffsAreReported)
{
    asset::VolumeFlipbookBakeSettings settings = SettingsFor(asset::kVolumeEmitterSourceName, 32);
    settings.source.emitter.burst = false;
    settings.source.emitter.count = 1000;
    settings.source.emitter.lifetime = 1.5f;

    const asset::VolumeFramingReport report = asset::AnalyzeVolumeFraming(settings);

    EXPECT_GT(report.maxLivePuffs, asset::kVolumeFillMaxPuffs);
    EXPECT_EQ(report.overflowFrames, 32);
    EXPECT_NE(report.frameIssues.front() & asset::kFramingTooManyPuffs, 0);
}

TEST_F(VolumeFlipbookFramingTest, CameraBasisLooksAlongPlusZAtZeroYaw)
{
    const asset::VolumeFlipbookCamera camera = asset::ComputeVolumeFlipbookCamera({});

    EXPECT_VEC3_NEAR(camera.forward, (math::Vector3{ 0.0f, 0.0f, 1.0f }), testkit::kTolerance);
    EXPECT_VEC3_NEAR(camera.right, (math::Vector3{ 1.0f, 0.0f, 0.0f }), testkit::kTolerance);
    EXPECT_VEC3_NEAR(camera.up, (math::Vector3{ 0.0f, 1.0f, 0.0f }), testkit::kTolerance);
    EXPECT_NEAR(camera.toLight.Length(), 1.0f, testkit::kTolerance);
}

TEST_F(VolumeFlipbookFramingTest, DefaultFireRampMatchesTheOldPiecewiseRamp)
{
    const asset::VolumeColorRamp ramp = asset::DefaultFireRamp();

    EXPECT_VEC3_NEAR(asset::EvaluateVolumeRamp(ramp, 0.0f), (math::Vector3{ 0.0f, 0.0f, 0.0f }), testkit::kTolerance);
    // 旧 FireRamp: t = 0.5 は c1 と c2 の中間。
    EXPECT_VEC3_NEAR(asset::EvaluateVolumeRamp(ramp, 0.5f), (math::Vector3{ 0.95f, 0.265f, 0.035f }),
                     testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(asset::EvaluateVolumeRamp(ramp, 1.0f), (math::Vector3{ 1.0f, 0.9f, 0.6f }), testkit::kTolerance);
}

TEST_F(VolumeFlipbookFramingTest, RampPositionsThatGoBackwardsAreHeldInPlace)
{
    asset::VolumeColorRamp ramp = asset::EvenVolumeRamp({ 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f },
                                                        { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f });
    // 3 点目が 2 点目より手前にある。2 点目の位置 (1/3) へ寄せて扱う。
    ramp.stops[2].position = 0.1f;

    const math::Vector3 color = asset::EvaluateVolumeRamp(ramp, 0.6f);

    EXPECT_NEAR(color.x, 0.0f, testkit::kLooseTolerance);
    EXPECT_GT(color.y, 0.0f);
    EXPECT_GT(color.z, 0.0f);
}

TEST_F(VolumeFlipbookFramingTest, SourceLookIsAppliedAndResetBetweenSources)
{
    asset::VolumeFlipbookBakeSettings settings = SettingsFor("BloodSpray", 32);
    asset::ApplyVolumeSourceLook(settings);
    const math::Vector3 blood = asset::EvaluateVolumeRamp(settings.albedoRamp, 1.0f);
    EXPECT_GT(blood.x, blood.y * 4.0f);

    settings.source.preset = "Puff";
    asset::ApplyVolumeSourceLook(settings);
    const asset::VolumeFlipbookBakeSettings defaults;
    EXPECT_VEC3_NEAR(asset::EvaluateVolumeRamp(settings.albedoRamp, 1.0f),
                     asset::EvaluateVolumeRamp(defaults.albedoRamp, 1.0f), testkit::kTolerance);
    EXPECT_NEAR(settings.liquid.extinction, defaults.liquid.extinction, testkit::kTolerance);
}

} // namespace fbzz::tests
