/// @file    FluidLiquid3DTests.cpp
/// @brief   液体ソルバーの 3D (奥行きを持つ PBF) と、ボリュームへの塗り方を固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// 2D の挙動は FluidSolverTests が固定している。

#include <TestKit/TestKit.hpp>

#include <Fluid/FluidSolver.hpp>
#include <Engine/Asset/VolumeFlipbookFluid.hpp>

#include <algorithm>
#include <cmath>

namespace fbzz::tests {
namespace {

fluid::FluidRecipe SplashRecipe()
{
    fluid::FluidRecipe recipe;
    recipe.kind = fluid::FluidKind::Liquid;
    recipe.liquid.maxParticles = 400;
    recipe.liquid.floor = true;
    recipe.liquid.floorHeight = -0.8f;
    fluid::FluidSource source;
    source.center = { 0.0f, -0.4f, 0.0f };
    source.size = { 0.1f, 0.1f, 0.1f };
    source.velocity = { 0.0f, 1.5f, 0.0f };
    source.spread = 0.8f;
    source.count = 300;
    source.startTime = 0.0f;
    source.duration = 0.0f;
    recipe.sources = { source };
    return recipe;
}

void Advance(fluid::FluidLiquidSolver& solver, float seconds)
{
    for (float t = 0.0f; t < seconds; t += 1.0f / 30.0f) solver.Advance(1.0f / 30.0f);
}

} // namespace

TEST(FluidLiquid3DTest, VolumetricSolveSpreadsInDepthAndStaysOnTheFloor)
{
    const fluid::FluidRecipe recipe = SplashRecipe();
    fluid::FluidLiquidSolver solver;
    solver.Reset(recipe, /*volumetric=*/true);
    EXPECT_TRUE(solver.IsVolumetric());
    Advance(solver, 0.6f);
    ASSERT_FALSE(solver.Particles().empty());
    float maxDepth = 0.0f;
    bool finite = true;
    for (const auto& particle : solver.Particles()) {
        maxDepth = (std::max)(maxDepth, std::fabs(particle.z));
        finite &= std::isfinite(particle.x) && std::isfinite(particle.y) && std::isfinite(particle.z);
        EXPECT_GE(particle.y, recipe.liquid.floorHeight - 1.0e-3f);
    }
    EXPECT_TRUE(finite);
    EXPECT_GT(maxDepth, 0.02f);
}

TEST(FluidLiquid3DTest, FlatSolveKeepsDepthAtZero)
{
    fluid::FluidRecipe recipe = SplashRecipe();
    /// @note 2D では奥行きの速度を見ない
    recipe.sources[0].velocity.z = 3.0f;
    fluid::FluidLiquidSolver solver;
    solver.Reset(recipe);
    Advance(solver, 0.3f);
    ASSERT_FALSE(solver.Particles().empty());
    for (const auto& particle : solver.Particles()) EXPECT_EQ(particle.z, 0.0f);
}

TEST(FluidLiquid3DTest, PackedVolumeMarksLiquidOnlyWhereParticlesAre)
{
    fluid::FluidLiquidSolver solver;
    solver.Reset(SplashRecipe(), /*volumetric=*/true);
    Advance(solver, 0.2f);
    asset::PackedFluidVolume volume;
    asset::PackLiquidVolume(solver, 16, 2.5f, 0.0f, volume);
    ASSERT_EQ(volume.resolution, 16);
    ASSERT_EQ(volume.medium.size(), 16u * 16u * 16u);
    int liquidCells = 0;
    bool consistent = true;
    for (const auto& cell : volume.medium) {
        if (cell.w == 1.0f) ++liquidCells;
        consistent &= (cell.w == 1.0f) == (cell.x > 0.0f);
    }
    EXPECT_GT(liquidCells, 0);
    EXPECT_LT(liquidCells, 16 * 16 * 16);
    EXPECT_TRUE(consistent);
}

TEST(FluidLiquid3DTest, ParticlesCarryTheirSourceColorKeyIntoTheVolume)
{
    /// @note 鍵 0 と鍵 1 の発生源を左右に離して置く。混ざる前なら、左の液は B = 0・右の液は B = 1。
    fluid::FluidRecipe recipe = SplashRecipe();
    recipe.liquid.maxParticles = 800;
    recipe.sources[0].center.x = -0.5f;
    recipe.sources[0].colorKey = 0.0f;
    fluid::FluidSource right = recipe.sources[0];
    right.center.x = 0.5f;
    right.colorKey = 1.0f;
    recipe.sources.push_back(right);

    fluid::FluidLiquidSolver solver;
    solver.Reset(recipe, /*volumetric=*/true);
    Advance(solver, 0.1f);
    ASSERT_FALSE(solver.Particles().empty());
    std::size_t keyedParticles = 0;
    for (const auto& particle : solver.Particles()) {
        if (particle.colorKey == 1.0f) ++keyedParticles;
        else EXPECT_EQ(particle.colorKey, 0.0f);
    }
    EXPECT_GT(keyedParticles, 0u);
    EXPECT_LT(keyedParticles, solver.Particles().size());

    asset::PackedFluidVolume volume;
    asset::PackLiquidVolume(solver, 16, 2.5f, 0.0f, volume);
    ASSERT_EQ(volume.medium.size(), 16u * 16u * 16u);
    int leftCells = 0;
    int rightCells = 0;
    bool separated = true;
    for (int z = 0; z < 16; ++z)
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) {
                const auto& cell = volume.medium[static_cast<std::size_t>(x + 16 * (y + 16 * z))];
                if (cell.w != 1.0f) continue;
                if (x <= 5) {
                    ++leftCells;
                    separated &= cell.z < 1.0e-6f;
                } else if (x >= 10) {
                    ++rightCells;
                    separated &= cell.z > 0.999f;
                }
            }
    EXPECT_GT(leftCells, 0);
    EXPECT_GT(rightCells, 0);
    EXPECT_TRUE(separated);
}

} // namespace fbzz::tests
