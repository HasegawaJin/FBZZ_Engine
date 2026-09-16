/// @file    FluidDeterminismTests.cpp
/// @brief   «同じ .fluid なら同じ絵» と «プレビュー = 焼きのそのコマ» を固定する
/// @author  Hasegawa Jin
/// @date    2026-09-14
///
/// AI は「値を変える → 絵を見る → 直す」を回す。見た絵と焼けた絵が別のシミュレーションだと、
/// この輪は回らない (実際に warmup の刻み方が経路ごとに 3 通りあり、ずれていた)。
/// ここで縛るのは 3 つ:
///   1. 同じ入力を 2 回解けば 1 画素も変わらない (seed が違えば変わる)
///   2. プレビューの n コマ目は、焼きの n コマ目と完全に同じ (ループの混ぜ込みも含む)
///   3. .fluid は書くたびに同じバイト列になる (読み書きで差分が汚れない)

#include <TestKit/TestKit.hpp>
#include <TestKit/TempDir.hpp>

#include <Engine/Asset/BakeFingerprint.hpp>
#include <Engine/Asset/FluidBaker.hpp>
#include <Engine/Asset/FluidRecipe.hpp>
#include <Engine/Asset/FluidStepping.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <cstdint>
#include <numeric>
#include <span>
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {

/// 秒で回せる大きさにした煙。コマ数も解像度も小さいが、刻みの規則は本番と同じ。
asset::FluidRecipe SmallGasRecipe()
{
    asset::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::Smoke);
    recipe.output.frameSize = 32;
    recipe.output.columns = 2;
    recipe.output.rows = 2;
    recipe.output.duration = 0.5f;
    recipe.output.warmup = 0.2f;
    recipe.output.substeps = 3;
    recipe.output.supersampling = 1;
    recipe.output.loop = false;
    recipe.output.vectorField = false;
    return recipe;
}

std::string FrameFingerprint(const asset::FluidFrameImage& image)
{
    asset::BakeFingerprint fingerprint;
    fingerprint.Add(std::span<const float>(image.rgba));
    fingerprint.Add(std::span<const float>(image.motion));
    return fingerprint.Finish();
}

/// frames コマを焼きの経路で解いて、コマごとの指紋を返す。
std::vector<std::string> BakeFingerprints(const asset::FluidRecipe& recipe, const std::vector<int>& frames)
{
    std::vector<asset::FluidFrameImage> images;
    if (!asset::RenderFluidBakeFrames(recipe, frames, 32, images)) return {};
    std::vector<std::string> out;
    out.reserve(images.size());
    for (const asset::FluidFrameImage& image : images) out.push_back(FrameFingerprint(image));
    return out;
}

std::vector<int> AllFrames(const asset::FluidRecipe& recipe)
{
    const asset::FluidStepPlan plan = asset::MakeFluidStepPlan(recipe);
    std::vector<int> frames(static_cast<std::size_t>(plan.frameCount));
    std::iota(frames.begin(), frames.end(), 0);
    return frames;
}

} // namespace

// ── 刻みの数え方 ──

TEST(FluidSteppingTest, WarmupIsCountedInWholeFrames)
{
    asset::FluidRecipe recipe = SmallGasRecipe();
    const asset::FluidStepPlan plan = asset::MakeFluidStepPlan(recipe);
    // 0.5 秒を 4 コマ = 1 コマ 0.125 秒。warmup 0.2 秒は 2 コマ (切り上げ)。
    EXPECT_FLOAT_EQ(plan.frameDt, 0.125f);
    EXPECT_EQ(plan.frameCount, 4);
    EXPECT_EQ(plan.warmupFrames, 2);
    EXPECT_EQ(plan.substeps, 3);
}

TEST(FluidSteppingTest, ExactWarmupDoesNotGainAnExtraFrame)
{
    // ちょうど割り切れる warmup で 1 コマ余分に数えると、GPU と CPU で焼き始めがずれる。
    EXPECT_EQ(asset::FluidWarmupFrames(0.25f, 0.125f), 2);
    EXPECT_EQ(asset::FluidWarmupFrames(0.26f, 0.125f), 3);
    EXPECT_EQ(asset::FluidWarmupFrames(0.0f, 0.125f), 0);
}

TEST(FluidSteppingTest, TimeSnapsToAFrameBoundary)
{
    const asset::FluidStepPlan plan = asset::MakeFluidStepPlan(SmallGasRecipe());
    EXPECT_EQ(plan.FrameOfTime(0.0f), 0);
    EXPECT_EQ(plan.FrameOfTime(0.13f), 1);
    // コマ数を超える秒は最後のコマで止める (存在しないコマを指さない)。
    EXPECT_EQ(plan.FrameOfTime(99.0f), plan.frameCount - 1);
}

// ── 同じ入力 → 同じ絵 ──

TEST(FluidDeterminismTest, SameRecipeGivesTheSamePixelsTwice)
{
    const asset::FluidRecipe recipe = SmallGasRecipe();
    const std::vector<int> frames = AllFrames(recipe);
    const std::vector<std::string> first = BakeFingerprints(recipe, frames);
    const std::vector<std::string> second = BakeFingerprints(recipe, frames);
    ASSERT_EQ(first.size(), frames.size());
    EXPECT_EQ(first, second);
}

TEST(FluidDeterminismTest, ChangingTheSeedChangesThePicture)
{
    asset::FluidRecipe recipe = SmallGasRecipe();
    const std::vector<int> frames = AllFrames(recipe);
    const std::vector<std::string> base = BakeFingerprints(recipe, frames);
    recipe.seed += 1;
    const std::vector<std::string> other = BakeFingerprints(recipe, frames);
    ASSERT_EQ(base.size(), other.size());
    EXPECT_NE(base.back(), other.back());
}

TEST(FluidDeterminismTest, LiquidIsDeterministicToo)
{
    asset::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::WaterSplash);
    recipe.output.frameSize = 32;
    recipe.output.columns = 2;
    recipe.output.rows = 1;
    recipe.output.duration = 0.3f;
    recipe.output.warmup = 0.1f;
    recipe.output.supersampling = 1;
    recipe.output.loop = false;
    const std::vector<int> frames = AllFrames(recipe);
    EXPECT_EQ(BakeFingerprints(recipe, frames), BakeFingerprints(recipe, frames));
}

// ── プレビュー = 焼きのそのコマ ──

TEST(FluidDeterminismTest, PreviewFrameMatchesTheBakedFrame)
{
    const asset::FluidRecipe recipe = SmallGasRecipe();
    const std::vector<std::string> baked = BakeFingerprints(recipe, AllFrames(recipe));
    ASSERT_EQ(baked.size(), 4u);
    // 1 コマだけ頼む道 (プレビュー) と、全コマ焼く道が同じ絵を出すこと。
    for (int frame = 0; frame < 4; ++frame) {
        const std::vector<std::string> single = BakeFingerprints(recipe, { frame });
        ASSERT_EQ(single.size(), 1u);
        EXPECT_EQ(single.front(), baked[static_cast<std::size_t>(frame)]) << "frame " << frame;
    }
}

TEST(FluidDeterminismTest, PreviewMatchesTheBakedFrameWhenLooping)
{
    // ループの先頭コマは «末尾の続き» と混ぜたものが焼かれる。プレビューがこれを再現しないと、
    // 先頭の数コマだけ «見た絵と違うもの» が焼ける。
    asset::FluidRecipe recipe = SmallGasRecipe();
    recipe.output.loop = true;
    const asset::FluidStepPlan plan = asset::MakeFluidStepPlan(recipe);
    ASSERT_GT(plan.loopOverlap, 0);
    const std::vector<std::string> baked = BakeFingerprints(recipe, AllFrames(recipe));
    ASSERT_EQ(baked.size(), static_cast<std::size_t>(plan.frameCount));
    const std::vector<std::string> head = BakeFingerprints(recipe, { 0 });
    ASSERT_EQ(head.size(), 1u);
    EXPECT_EQ(head.front(), baked.front());
}

TEST(FluidDeterminismTest, SupersamplingAppliesToSingleFramesToo)
{
    // 焼きだけ超解像が掛かっていたころは、プレビューより焼きの方が滑らかだった。
    asset::FluidRecipe recipe = SmallGasRecipe();
    recipe.output.supersampling = 2;
    const std::vector<std::string> baked = BakeFingerprints(recipe, AllFrames(recipe));
    const std::vector<std::string> single = BakeFingerprints(recipe, { 2 });
    ASSERT_EQ(single.size(), 1u);
    EXPECT_EQ(single.front(), baked[2]);
}

// ── 指紋そのもの ──

TEST(BakeFingerprintTest, DifferentBytesGiveDifferentDigests)
{
    const std::vector<std::uint8_t> a{ 1, 2, 3, 4 };
    const std::vector<std::uint8_t> b{ 1, 2, 3, 5 };
    EXPECT_EQ(asset::BakeFingerprintOf(a).size(), 16u);
    EXPECT_EQ(asset::BakeFingerprintOf(a), asset::BakeFingerprintOf(a));
    EXPECT_NE(asset::BakeFingerprintOf(a), asset::BakeFingerprintOf(b));
}

TEST(BakeFingerprintTest, NegativeZeroHashesAsZero)
{
    // 同じ絵が «-0 が混ざっているかどうか» で別の指紋になると、変化の検出が嘘になる。
    const std::vector<float> positive{ 0.0f, 1.0f };
    const std::vector<float> negative{ -0.0f, 1.0f };
    EXPECT_EQ(asset::BakeFingerprintOf(positive), asset::BakeFingerprintOf(negative));
}

// ── .fluid の書き出し ──

TEST(FluidRecipeWriteTest, SavingTwiceGivesTheSameBytes)
{
    testkit::TempDir temp{ "fluiddeterminism" };
    ASSERT_TRUE(temp.IsValid());
    const std::string path = util::FileSystem::PathToUtf8(temp.File("Determinism.fluid"));
    const asset::FluidRecipe recipe = SmallGasRecipe();
    ASSERT_TRUE(asset::SaveFluidRecipe(path, recipe));
    std::string first;
    ASSERT_TRUE(util::FileSystem::ReadText(path, first));
    ASSERT_TRUE(asset::SaveFluidRecipe(path, recipe));
    std::string second;
    ASSERT_TRUE(util::FileSystem::ReadText(path, second));
    EXPECT_EQ(first, second);
}

TEST(FluidRecipeWriteTest, RoundTripKeepsTheBytesStable)
{
    // 読んで書き戻しただけで行が動くと、AI が 1 項目直した差分に無関係な行が混ざる。
    testkit::TempDir temp{ "fluiddeterminism" };
    ASSERT_TRUE(temp.IsValid());
    const std::string path = util::FileSystem::PathToUtf8(temp.File("RoundTrip.fluid"));
    asset::FluidRecipe recipe = SmallGasRecipe();
    recipe.gas.buoyancy = 0.1f;   // float では 0.10000000149011612 になる値
    recipe.render.opacity = 1.3f;
    ASSERT_TRUE(asset::SaveFluidRecipe(path, recipe));
    std::string first;
    ASSERT_TRUE(util::FileSystem::ReadText(path, first));
    EXPECT_NE(first.find("buoyancy = 0.1"), std::string::npos) << first;

    asset::FluidRecipe loaded;
    ASSERT_TRUE(asset::LoadFluidRecipe(path, loaded));
    ASSERT_TRUE(asset::SaveFluidRecipe(path, loaded));
    std::string second;
    ASSERT_TRUE(util::FileSystem::ReadText(path, second));
    EXPECT_EQ(first, second);
}

} // namespace fbzz::tests
