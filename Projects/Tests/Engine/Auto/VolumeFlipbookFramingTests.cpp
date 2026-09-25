/// @file    VolumeFlipbookFramingTests.cpp
/// @brief   焼く前の構図検査 (箱やタイルの縁で煙が切れないか) の判定と、Look の既定値を固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-11
/// @note 既定のプリセットで警告が鳴ると、利用者は警告を読まなくなる。逆に鳴るべき設定で
/// @note 黙っていると、縁で切れた «四角い板» の煙を何十コマも焼いてしまう。両側を押さえる。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Asset/VolumeFlipbookBaker.hpp>
#include <Engine/Asset/FluidBaker.hpp>
#include <Fluid/FluidStepping.hpp>

#include <algorithm>
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

/// @note 2D の FluidBaker (FluidBaker.cpp の case FluidShading::Distortion) の写し。
/// @note Engine のテストからは焼きの中の静的関数に届かないので、定数ごとここへ書き写している。
math::Vector2 FlatDistortionRg(math::Vector2 velocity)
{
    constexpr float kFlatScale = 0.42f;
    const float speed = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y);
    const float scale = speed > 1.0e-5f ? std::min(speed, 1.0f) / speed * kFlatScale : 0.0f;
    return { 0.5f + velocity.x * scale, 0.5f - velocity.y * scale };
}

} /// @note namespace

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
    /// @note 旧 FireRamp: t = 0.5 は c1 と c2 の中間。
    EXPECT_VEC3_NEAR(asset::EvaluateVolumeRamp(ramp, 0.5f), (math::Vector3{ 0.95f, 0.265f, 0.035f }),
                     testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(asset::EvaluateVolumeRamp(ramp, 1.0f), (math::Vector3{ 1.0f, 0.9f, 0.6f }), testkit::kTolerance);
}

TEST_F(VolumeFlipbookFramingTest, RampPositionsThatGoBackwardsAreHeldInPlace)
{
    asset::VolumeColorRamp ramp = asset::EvenVolumeRamp({ 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f },
                                                        { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f });
    /// @note 3 点目が 2 点目より手前にある。2 点目の位置 (1/3) へ寄せて扱う。
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

/// @note 流体のループは 2D と同じ割合から重ねコマ数を求める。
TEST_F(VolumeFlipbookFramingTest, FluidLoopOverlapMatchesTheFlatBaker)
{
    asset::VolumeFlipbookBakeSettings settings;
    settings.sourceKind = asset::VolumeSourceKind::Fluid;
    settings.source.frameCount = 32;
    EXPECT_FALSE(asset::VolumeBakeLoops(settings));
    EXPECT_EQ(asset::VolumeLoopOverlapFrames(settings), 0);

    settings.fluidLoop = true;
    EXPECT_TRUE(asset::VolumeBakeLoops(settings));
    EXPECT_EQ(asset::VolumeLoopOverlapFrames(settings), 8);
    settings.source.frameCount = 3;
    EXPECT_EQ(asset::VolumeLoopOverlapFrames(settings), 1);
    settings.source.frameCount = 32;
    settings.fluidLoopBlendFraction = 0.125f;
    EXPECT_EQ(asset::VolumeLoopOverlapFrames(settings), 4);
    settings.fluidLoopBlendFraction = 0.0f;
    EXPECT_EQ(asset::VolumeLoopOverlapFrames(settings), 0);
    settings.fluidLoopBlendFraction = 0.5f;
    EXPECT_EQ(asset::VolumeLoopOverlapFrames(settings), 16);

    /// @note 解析ソースのループは puff の湧き方で閉じる。流体の重ねは使わない。
    settings.sourceKind = asset::VolumeSourceKind::Analytic;
    settings.source.frameCount = 32;
    EXPECT_EQ(asset::VolumeLoopOverlapFrames(settings), 0);
}

/// @note 0 コマ目ほど «最終コマの続き» を多く混ぜ、重ねの最後で元のコマへ戻る。
TEST_F(VolumeFlipbookFramingTest, LoopKeepWeightRisesFromTheTailToTheHead)
{
    constexpr int overlap = 4;
    EXPECT_FLOAT_EQ(asset::VolumeLoopKeepWeight(0, overlap), 0.0f);
    EXPECT_NEAR(asset::VolumeLoopKeepWeight(overlap - 1, overlap), 0.84375f, testkit::kTolerance);
    EXPECT_FLOAT_EQ(asset::VolumeLoopKeepWeight(overlap, overlap), 1.0f);
    EXPECT_FLOAT_EQ(asset::VolumeLoopKeepWeight(0, 1), 0.0f);
    EXPECT_FLOAT_EQ(asset::VolumeLoopKeepWeight(1, 1), 1.0f);
    for (int i = 1; i < overlap; ++i)
        EXPECT_GT(asset::VolumeLoopKeepWeight(i, overlap), asset::VolumeLoopKeepWeight(i - 1, overlap));
    EXPECT_FLOAT_EQ(asset::VolumeLoopKeepWeight(0, 0), 1.0f);
}

TEST_F(VolumeFlipbookFramingTest, LoopBlendFinishesWithoutDroppingResidualTailAbruptly)
{
    constexpr int overlap = 16;
    const float remainingTail = 1.0f - asset::VolumeLoopKeepWeight(overlap - 1, overlap);
    EXPECT_LT(remainingTail, 0.02f);
    EXPECT_GT(remainingTail, 0.0f);
    for (int frame = 0; frame <= overlap; ++frame)
        EXPECT_FLOAT_EQ(asset::VolumeLoopKeepWeight(frame, overlap),
                        fluid::FluidLoopKeepWeight(frame, overlap));
}

TEST_F(VolumeFlipbookFramingTest, FlatLoopBeginsWithTheActualContinuationIncludingMotion)
{
    fluid::FluidRecipe recipe;
    recipe.gas.resolution = 16;
    recipe.gas.pressureIterations = 4;
    recipe.output.columns = 4;
    recipe.output.rows = 2;
    recipe.output.duration = 0.4f;
    recipe.output.warmup = 0.0f;
    recipe.output.supersampling = 1;
    recipe.output.loop = true;
    recipe.output.loopBlendFraction = 0.5f;
    recipe.sources.emplace_back();
    const int frames[] = { 0, 8 };
    std::vector<asset::FluidFrameImage> images;
    ASSERT_TRUE(asset::RenderFluidBakeFrames(recipe, frames, 16, images));
    ASSERT_EQ(images.size(), 2u);
    ASSERT_FALSE(images[1].rgba.empty());
    EXPECT_TRUE(std::any_of(images[1].rgba.begin(), images[1].rgba.end(),
                           [](float value) { return value > 0.0f; }));
    EXPECT_EQ(images[0].rgba, images[1].rgba);
    EXPECT_EQ(images[0].motion, images[1].motion);
}

/// @note 2D の Distortion と同じ符号化: RG = 0.5 + (右, 下) の変位、B = 0.5、A = 覆い。
TEST_F(VolumeFlipbookFramingTest, DistortionEncodingMatchesTheFlatBakerConvention)
{
    const math::Vector4 still = asset::EncodeVolumeDistortion({ 0.0f, 0.0f }, 0.25f, 0.42f);
    EXPECT_NEAR(still.x, 0.5f, testkit::kTolerance);
    EXPECT_NEAR(still.y, 0.5f, testkit::kTolerance);
    EXPECT_NEAR(still.z, 0.5f, testkit::kTolerance);
    EXPECT_NEAR(still.w, 0.25f, testkit::kTolerance);

    /// @note 上へ動く流れは画像の −V。G は減る (2D の g = 0.5 − vy と同じ)。
    const math::Vector4 upRight = asset::EncodeVolumeDistortion({ 0.5f, 0.5f }, 1.0f, 0.42f);
    EXPECT_NEAR(upRight.x, 0.5f + 0.21f, testkit::kTolerance);
    EXPECT_NEAR(upRight.y, 0.5f - 0.21f, testkit::kTolerance);

    /// @note 速すぎる流れは向きを保ったまま長さ 0.5 で頭打ち。
    const math::Vector4 fast = asset::EncodeVolumeDistortion({ 10.0f, 0.0f }, 1.0f, 1.0f);
    EXPECT_NEAR(fast.x, 1.0f, testkit::kTolerance);
    EXPECT_NEAR(fast.y, 0.5f, testkit::kTolerance);
}

/// @note 速さ 3 を含めるのは回帰検出のため。3D が速さをそのまま掛けていた頃は速さ 1 を超えると
/// @note 2D と乖離し、符号化上限 (長さ 0.5) に当たるまで歪みが伸び続けていた。
TEST_F(VolumeFlipbookFramingTest, DistortionEncodingAgreesWithTheFlatBakerAtEverySpeed)
{
    /// @note MakeVolumeBakeSettings が 2D に合わせて渡す倍率。
    constexpr float kScale = 0.42f;

    for (const float speed : { 0.5f, 1.0f, 3.0f }) {
        /// @note 斜めでも向きが保たれるか見るための右上・左上向き。
        const float axis = speed * 0.70710678f;
        const math::Vector2 samples[] = { { speed, 0.0f }, { 0.0f, speed }, { axis, axis }, { -axis, axis } };
        for (const math::Vector2& velocity : samples) {
            const math::Vector2 flat = FlatDistortionRg(velocity);
            const math::Vector4 volume = asset::EncodeVolumeDistortion(velocity, 1.0f, kScale);
            EXPECT_NEAR(volume.x, flat.x, testkit::kTolerance) << "speed=" << speed;
            EXPECT_NEAR(volume.y, flat.y, testkit::kTolerance) << "speed=" << speed;
            EXPECT_NEAR(volume.z, 0.5f, testkit::kTolerance) << "speed=" << speed;
        }
    }

    /// @note 速さ 1 と 3 は同じ長さ (= scale) で、伸び続けない。
    const math::Vector4 one = asset::EncodeVolumeDistortion({ 1.0f, 0.0f }, 1.0f, kScale);
    const math::Vector4 three = asset::EncodeVolumeDistortion({ 3.0f, 0.0f }, 1.0f, kScale);
    EXPECT_NEAR(one.x, 0.5f + kScale, testkit::kTolerance);
    EXPECT_NEAR(three.x, one.x, testkit::kTolerance);
    EXPECT_NEAR(three.y, 0.5f, testkit::kTolerance);
}

} /// @note namespace fbzz::tests
