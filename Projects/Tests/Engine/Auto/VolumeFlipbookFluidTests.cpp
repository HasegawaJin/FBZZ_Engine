/// @file    VolumeFlipbookFluidTests.cpp
/// @brief   3D 流体 → Volume Flipbook Baker の入力 (PackFluidVolume / FluidVolumeStream) を固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// 並び (x が最も速い) と倍率がずれると、GPU へ写した煙が «裏返る» «薄い» になり、
/// レイマーチの絵を見比べるまで気付けない。コマの数え方がずれると 2D ベイクと 1 コマずれる。

#include <TestKit/TestKit.hpp>

#include <Engine/Asset/VolumeFlipbookFluid.hpp>

#include <algorithm>
#include <chrono>
#include <thread>

namespace fbzz::tests {
namespace {

asset::FluidRecipe SmokeRecipe()
{
    asset::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::Smoke);
    recipe.output.warmup = 0.0f;
    recipe.output.substeps = 1;
    return recipe;
}

bool WaitForFrame(asset::FluidVolumeStream& stream, asset::PackedFluidVolume& out)
{
    for (int attempt = 0; attempt < 4000; ++attempt) {
        if (stream.Poll(out)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

asset::PackedFluidVolume SolveByHand(const asset::FluidRecipe& recipe, int resolution, int advances, float frameDt)
{
    asset::FluidGasSolver solver;
    solver.Reset(recipe, resolution, resolution, resolution);
    for (int i = 0; i < advances; ++i) solver.Advance(frameDt);
    asset::PackedFluidVolume packed;
    asset::PackFluidVolume(solver, { 1.0f, asset::FluidRecipeTemperatureScale(recipe) }, packed);
    return packed;
}

} // namespace

TEST(VolumeFlipbookFluidTest, PackKeepsTheGridLayoutAndScales)
{
    const asset::FluidRecipe recipe = SmokeRecipe();
    asset::FluidGasSolver solver;
    solver.Reset(recipe, 8, 8, 8);
    for (int i = 0; i < 6; ++i) solver.Advance(1.0f / 24.0f);

    asset::PackedFluidVolume packed;
    asset::PackFluidVolume(solver, { 0.5f, 2.0f }, packed);
    ASSERT_EQ(packed.resolution, 8);
    ASSERT_EQ(packed.medium.size(), 512u);
    ASSERT_EQ(packed.velocity.size(), 512u);
    // VolumeUpload.cs.hlsl は x + n·(y + n·z) で引く。ソルバーの並びがこれと同じであること。
    EXPECT_EQ(solver.Index(1, 2, 3), static_cast<std::size_t>(1 + 8 * (2 + 8 * 3)));

    float total = 0.0f;
    bool matches = true;
    for (std::size_t i = 0; i < packed.medium.size(); ++i) {
        matches &= std::abs(packed.medium[i].x - (std::max)(solver.Density()[i], 0.0f) * 0.5f) < 1.0e-6f;
        matches &= std::abs(packed.medium[i].y - (std::max)(solver.Temperature()[i], 0.0f) * 2.0f) < 1.0e-6f;
        matches &= packed.medium[i].z == solver.ColorKey()[i] && packed.medium[i].w == 0.0f;
        matches &= std::abs(packed.velocity[i].x - solver.VelocityX()[i]) < 1.0e-6f;
        matches &= std::abs(packed.velocity[i].y - solver.VelocityY()[i]) < 1.0e-6f;
        matches &= std::abs(packed.velocity[i].z - solver.VelocityZ()[i]) < 1.0e-6f;
        total += packed.medium[i].x;
    }
    EXPECT_TRUE(matches);
    EXPECT_GT(total, 0.0f);
}

TEST(VolumeFlipbookFluidTest, PackCarriesTheGasColorKeyInB)
{
    // レイマーチは B を albedoRamp の鍵として読む。鍵 1 の煙だけなら、煙のある所は全部 1・無い所は 0。
    asset::FluidRecipe recipe = SmokeRecipe();
    for (auto& source : recipe.sources) source.colorKey = 1.0f;
    asset::FluidGasSolver solver;
    solver.Reset(recipe, 8, 8, 8);
    for (int i = 0; i < 6; ++i) solver.Advance(1.0f / 24.0f);

    asset::PackedFluidVolume packed;
    asset::PackFluidVolume(solver, { 1.0f, 1.0f }, packed);
    ASSERT_EQ(packed.medium.size(), 512u);
    int smokeCells = 0;
    bool keyed = true;
    for (std::size_t i = 0; i < packed.medium.size(); ++i) {
        const float density = solver.Density()[i];
        if (density > 1.0e-3f) {
            ++smokeCells;
            keyed &= packed.medium[i].z > 0.99f;
        } else if (density == 0.0f) {
            keyed &= packed.medium[i].z == 0.0f;
        }
    }
    EXPECT_GT(smokeCells, 0);
    EXPECT_TRUE(keyed);
}

TEST(VolumeFlipbookFluidTest, NonCubicGridPacksNothing)
{
    asset::FluidGasSolver solver;
    solver.Reset(SmokeRecipe(), 8, 8, 1);
    asset::PackedFluidVolume packed;
    asset::PackFluidVolume(solver, {}, packed);
    EXPECT_EQ(packed.resolution, 0);
    EXPECT_TRUE(packed.medium.empty());
}

TEST(VolumeFlipbookFluidTest, TemperatureScaleMakesTheHottestSourceOne)
{
    asset::FluidRecipe recipe;
    EXPECT_NEAR(asset::FluidRecipeTemperatureScale(recipe), 1.0f, 1.0e-6f);
    asset::FluidSource warm;
    warm.temperature = 2.0f;
    asset::FluidSource hot;
    hot.temperature = 4.0f;
    recipe.sources = { warm, hot };
    EXPECT_NEAR(asset::FluidRecipeTemperatureScale(recipe), 0.25f, 1.0e-6f);
    // 切ってある発生源は注がないので、倍率の基準にもしない。
    recipe.sources[1].enabled = false;
    EXPECT_NEAR(asset::FluidRecipeTemperatureScale(recipe), 0.5f, 1.0e-6f);
}

TEST(VolumeFlipbookFluidTest, StreamCountsFramesLikeThe2DBake)
{
    const asset::FluidRecipe recipe = SmokeRecipe();
    constexpr float kFrameDt = 1.0f / 24.0f;
    asset::FluidVolumeStream stream;
    std::string error;
    ASSERT_TRUE(stream.Open(recipe, 8, kFrameDt, 1.0f, error));

    // frame コマ目 = frame + 1 回進めた状態。
    asset::PackedFluidVolume frame1;
    ASSERT_TRUE(stream.Request(1));
    EXPECT_FALSE(stream.Request(2));   // 解いている最中は受け付けない
    ASSERT_TRUE(WaitForFrame(stream, frame1));
    EXPECT_EQ(frame1.frame, 1);
    const asset::PackedFluidVolume expected1 = SolveByHand(recipe, 8, 2, kFrameDt);
    ASSERT_EQ(frame1.medium.size(), expected1.medium.size());
    bool same = true;
    for (std::size_t i = 0; i < expected1.medium.size(); ++i)
        same &= frame1.medium[i].x == expected1.medium[i].x && frame1.velocity[i].y == expected1.velocity[i].y;
    EXPECT_TRUE(same);

    // 手前のコマへ戻ると最初から解き直す。
    asset::PackedFluidVolume frame0;
    ASSERT_TRUE(stream.Request(0));
    ASSERT_TRUE(WaitForFrame(stream, frame0));
    const asset::PackedFluidVolume expected0 = SolveByHand(recipe, 8, 1, kFrameDt);
    same = frame0.medium.size() == expected0.medium.size();
    for (std::size_t i = 0; same && i < expected0.medium.size(); ++i) same &= frame0.medium[i].x == expected0.medium[i].x;
    EXPECT_TRUE(same);
}

TEST(VolumeFlipbookFluidTest, LiquidRecipeSolvesIn3D)
{
    asset::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::WaterSplash);
    recipe.output.warmup = 0.0f;
    recipe.output.substeps = 1;
    recipe.liquid.maxParticles = 300;
    for (auto& source : recipe.sources) source.count = (std::min)(source.count, 150);
    asset::FluidVolumeStream stream;
    std::string error;
    ASSERT_TRUE(stream.Open(recipe, 16, 1.0f / 24.0f, 1.0f, error));
    EXPECT_TRUE(stream.IsOpen());
    ASSERT_TRUE(stream.Request(3));
    asset::PackedFluidVolume volume;
    ASSERT_TRUE(WaitForFrame(stream, volume));
    ASSERT_EQ(volume.resolution, 16);
    // 液体は «液体の割合» (A) を 1 で塗る。レイマーチはそれを見て液面として描く。
    bool anyLiquid = false;
    for (const auto& cell : volume.medium) anyLiquid |= cell.w == 1.0f && cell.x > 0.0f;
    EXPECT_TRUE(anyLiquid);
}

} // namespace fbzz::tests
