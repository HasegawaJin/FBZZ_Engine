/// @file    VolumeFlipbookFramingTests.cpp
/// @brief   焼く前の構図検査 (箱やタイルの縁で煙が切れないか) の判定を固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// 既定のプリセットで警告が鳴ると、利用者は警告を読まなくなる。逆に鳴るべき設定で
/// 黙っていると、縁で切れた «四角い板» の煙を何十コマも焼いてしまう。両側を押さえる。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Asset/VolumeFlipbookBaker.hpp>

#include <cmath>

namespace fbzz::tests {
namespace {

class VolumeFlipbookFramingTest : public testkit::EngineFixture {};

asset::VolumeFlipbookBakeSettings SettingsFor(asset::VolumeFlipbookPreset preset, int frames)
{
    asset::VolumeFlipbookBakeSettings settings;
    settings.source.preset = preset;
    settings.source.frameCount = frames;
    return settings;
}

} // namespace

TEST_F(VolumeFlipbookFramingTest, DefaultPresetsStayInsideTheBoxAndTheTile)
{
    for (const auto preset : { asset::VolumeFlipbookPreset::Puff, asset::VolumeFlipbookPreset::RisingPlume,
                               asset::VolumeFlipbookPreset::Fireball }) {
        for (const int frames : { 32, 64, 128 }) {
            const asset::VolumeFramingReport report = asset::AnalyzeVolumeFraming(SettingsFor(preset, frames));

            ASSERT_EQ(report.frameIssues.size(), static_cast<std::size_t>(frames));
            EXPECT_EQ(report.boxCutFrames, 0) << "preset=" << static_cast<int>(preset) << " frames=" << frames;
            EXPECT_EQ(report.tileCutFrames, 0) << "preset=" << static_cast<int>(preset) << " frames=" << frames;
        }
    }
}

TEST_F(VolumeFlipbookFramingTest, NarrowFramingIsReportedAsCutByTheTileEdge)
{
    asset::VolumeFlipbookBakeSettings settings = SettingsFor(asset::VolumeFlipbookPreset::Puff, 32);
    settings.halfExtent = 0.5f;

    const asset::VolumeFramingReport report = asset::AnalyzeVolumeFraming(settings);

    EXPECT_EQ(report.tileCutFrames, 32);
    EXPECT_EQ(report.boxCutFrames, 0);
    EXPECT_NE(report.frameIssues.front() & asset::kFramingCutByTileEdge, 0);
}

TEST_F(VolumeFlipbookFramingTest, CameraBasisLooksAlongPlusZAtZeroYaw)
{
    const asset::VolumeFlipbookCamera camera = asset::ComputeVolumeFlipbookCamera({});

    EXPECT_VEC3_NEAR(camera.forward, (math::Vector3{ 0.0f, 0.0f, 1.0f }), testkit::kTolerance);
    EXPECT_VEC3_NEAR(camera.right, (math::Vector3{ 1.0f, 0.0f, 0.0f }), testkit::kTolerance);
    EXPECT_VEC3_NEAR(camera.up, (math::Vector3{ 0.0f, 1.0f, 0.0f }), testkit::kTolerance);
    EXPECT_NEAR(camera.toLight.Length(), 1.0f, testkit::kTolerance);
}

} // namespace fbzz::tests
