/// @file    FluidGpuLiquidTests.cpp
/// @brief   GPU 液体ソルバーへ上げる値 (湧かせ方・核の定数・格子・部品・並べ替えの段) を固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// GPU の結果は読み戻さないので、CPU ソルバー (FluidLiquidSolver) の規則からのずれはここでしか捕まえられない。

#include <TestKit/TestKit.hpp>

#include <Engine/Asset/FluidGpuLiquidPack.hpp>
#include <Engine/Asset/FluidGpuLiquidSolver.hpp>
#include <Engine/Asset/FluidSolver.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace fbzz::tests {
namespace {

asset::FluidRecipe LiquidRecipe()
{
    asset::FluidRecipe recipe;
    recipe.kind = asset::FluidKind::Liquid;
    recipe.liquid.maxParticles = 100000;
    return recipe;
}

asset::FluidSource StillSphere(const math::Vector3& center, float radius, int count)
{
    asset::FluidSource source;
    source.shape = asset::FluidSourceShape::Sphere;
    source.center = center;
    source.size = { radius, radius, radius };
    source.velocity = { 0.0f, 0.0f, 0.0f };
    source.count = count;
    return source;
}

float DistanceTo(const asset::GpuLiquidSpawn& spawn, const math::Vector3& center)
{
    const float dx = spawn.positionTime[0] - center.x;
    const float dy = spawn.positionTime[1] - center.y;
    const float dz = spawn.positionTime[2] - center.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace

TEST(FluidGpuLiquidTest, CapacityIsTheSumOfEnabledCountsClamped)
{
    asset::FluidRecipe recipe = LiquidRecipe();
    const asset::FluidSource a = StillSphere({ 0.0f, 0.0f, 0.0f }, 0.2f, 300);
    const asset::FluidSource b = StillSphere({ 0.5f, 0.0f, 0.0f }, 0.2f, 200);
    asset::FluidSource off = a;
    off.enabled = false;
    off.count = 999;
    recipe.sources = { a, off, b };

    EXPECT_EQ(asset::GpuLiquidParticleCapacity(recipe, asset::FluidGpuLiquidSolver::kMaxParticles), 500);
    EXPECT_EQ(asset::GpuLiquidParticleCapacity(recipe, 120), 120);
    recipe.liquid.maxParticles = 50;
    EXPECT_EQ(asset::GpuLiquidParticleCapacity(recipe, asset::FluidGpuLiquidSolver::kMaxParticles), 50);
    EXPECT_EQ(asset::BuildGpuLiquidEmission(recipe, asset::FluidGpuLiquidSolver::kMaxParticles).size(), 50u);

    recipe.sources.clear();
    EXPECT_EQ(asset::GpuLiquidParticleCapacity(recipe, asset::FluidGpuLiquidSolver::kMaxParticles), 0);
    EXPECT_TRUE(asset::BuildGpuLiquidEmission(recipe, asset::FluidGpuLiquidSolver::kMaxParticles).empty());
}

TEST(FluidGpuLiquidTest, BurstSpawnsEveryParticleAtTheStartTime)
{
    asset::FluidRecipe recipe = LiquidRecipe();
    asset::FluidSource burst = StillSphere({ 0.0f, 0.0f, 0.0f }, 0.2f, 40);
    burst.startTime = 0.3f;
    burst.duration = 0.0f;
    recipe.sources = { burst };

    const auto spawns = asset::BuildGpuLiquidEmission(recipe, 1000);
    ASSERT_EQ(spawns.size(), 40u);
    for (const auto& spawn : spawns) EXPECT_EQ(spawn.positionTime[3], 0.3f);
}

TEST(FluidGpuLiquidTest, TimedSourceSpreadsSpawnsOverItsDuration)
{
    asset::FluidRecipe recipe = LiquidRecipe();
    asset::FluidSource stream = StillSphere({ 0.0f, 0.0f, 0.0f }, 0.2f, 4);
    stream.startTime = 0.5f;
    stream.duration = 2.0f;
    recipe.sources = { stream };

    // CPU は «count × 経過割合» を切り捨てた数だけ出すので、k 番目は start + duration × (k + 1) / count で出る。
    const auto spawns = asset::BuildGpuLiquidEmission(recipe, 1000);
    ASSERT_EQ(spawns.size(), 4u);
    EXPECT_NEAR(spawns[0].positionTime[3], 1.0f, 1.0e-6f);
    EXPECT_NEAR(spawns[1].positionTime[3], 1.5f, 1.0e-6f);
    EXPECT_NEAR(spawns[2].positionTime[3], 2.0f, 1.0e-6f);
    EXPECT_NEAR(spawns[3].positionTime[3], 2.5f, 1.0e-6f);
}

TEST(FluidGpuLiquidTest, EarliestSpawnsTakeTheSlotsWhenTheyRunOut)
{
    asset::FluidRecipe recipe = LiquidRecipe();
    asset::FluidSource late = StillSphere({ 0.5f, 0.0f, 0.0f }, 0.2f, 10);
    late.startTime = 2.0f;
    asset::FluidSource early = StillSphere({ -0.5f, 0.0f, 0.0f }, 0.2f, 10);
    early.startTime = 0.0f;
    early.duration = 1.0f;
    recipe.sources = { late, early };

    const auto spawns = asset::BuildGpuLiquidEmission(recipe, 12);
    ASSERT_EQ(spawns.size(), 12u);
    for (std::size_t i = 1; i < spawns.size(); ++i)
        EXPECT_LE(spawns[i - 1].positionTime[3], spawns[i].positionTime[3]);
    // 先に出る 10 粒 (early) が全部入り、残りの 2 枠が late に回る。
    EXPECT_NEAR(spawns[0].positionTime[3], 0.1f, 1.0e-6f);
    EXPECT_NEAR(spawns[9].positionTime[3], 1.0f, 1.0e-6f);
    EXPECT_EQ(spawns[10].positionTime[3], 2.0f);
    EXPECT_EQ(spawns[11].positionTime[3], 2.0f);
}

TEST(FluidGpuLiquidTest, SphereSpawnsStayInsideTheShapeAndCarryTheColorKey)
{
    asset::FluidRecipe recipe = LiquidRecipe();
    const math::Vector3 center = { 0.1f, -0.3f, 0.2f };
    asset::FluidSource ball = StillSphere(center, 0.25f, 50);
    ball.colorKey = 0.7f;
    recipe.sources = { ball };

    const auto spawns = asset::BuildGpuLiquidEmission(recipe, 1000);
    ASSERT_EQ(spawns.size(), 50u);
    for (const auto& spawn : spawns) {
        EXPECT_LE(DistanceTo(spawn, center), 0.25f + 1.0e-5f);
        EXPECT_EQ(spawn.velocityKey[0], 0.0f);
        EXPECT_EQ(spawn.velocityKey[1], 0.0f);
        EXPECT_EQ(spawn.velocityKey[2], 0.0f);
        EXPECT_EQ(spawn.velocityKey[3], 0.7f);
    }
}

TEST(FluidGpuLiquidTest, BoxSpawnsStayInsideTheBoxAndLaunchWithTheSourceVelocity)
{
    asset::FluidRecipe recipe = LiquidRecipe();
    asset::FluidSource box;
    box.shape = asset::FluidSourceShape::Box;
    box.center = { 0.0f, -0.5f, 0.0f };
    box.size = { 0.3f, 0.1f, 0.2f };
    box.velocity = { 0.0f, 2.0f, 0.0f };
    box.spread = 0.0f;
    box.count = 60;
    recipe.sources = { box };

    const auto spawns = asset::BuildGpuLiquidEmission(recipe, 1000);
    ASSERT_EQ(spawns.size(), 60u);
    for (const auto& spawn : spawns) {
        EXPECT_LE(std::fabs(spawn.positionTime[0] - 0.0f), 0.3f + 1.0e-5f);
        EXPECT_LE(std::fabs(spawn.positionTime[1] + 0.5f), 0.1f + 1.0e-5f);
        EXPECT_LE(std::fabs(spawn.positionTime[2] - 0.0f), 0.2f + 1.0e-5f);
        // spread = 0 ならばらつきは 0。撃ち出す速度そのまま。
        EXPECT_FLOAT_EQ(spawn.velocityKey[0], 0.0f);
        EXPECT_FLOAT_EQ(spawn.velocityKey[1], 2.0f);
        EXPECT_FLOAT_EQ(spawn.velocityKey[2], 0.0f);
    }
}

TEST(FluidGpuLiquidTest, SameSeedGivesTheSameSpawnsAndAnotherSeedDiffers)
{
    asset::FluidRecipe recipe = LiquidRecipe();
    asset::FluidSource ball = StillSphere({ 0.0f, 0.0f, 0.0f }, 0.2f, 30);
    ball.velocity = { 0.5f, 1.0f, 0.0f };
    ball.spread = 0.5f;
    recipe.sources = { ball };
    recipe.seed = 7;

    const auto a = asset::BuildGpuLiquidEmission(recipe, 1000);
    const auto b = asset::BuildGpuLiquidEmission(recipe, 1000);
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        for (int k = 0; k < 4; ++k) {
            EXPECT_EQ(a[i].positionTime[k], b[i].positionTime[k]);
            EXPECT_EQ(a[i].velocityKey[k], b[i].velocityKey[k]);
        }
    }

    recipe.seed = 8;
    const auto c = asset::BuildGpuLiquidEmission(recipe, 1000);
    ASSERT_EQ(c.size(), a.size());
    bool differs = false;
    for (std::size_t i = 0; i < a.size(); ++i)
        differs |= a[i].positionTime[0] != c[i].positionTime[0] || a[i].velocityKey[0] != c[i].velocityKey[0];
    EXPECT_TRUE(differs);
}

TEST(FluidGpuLiquidTest, OvercrowdedSourceIsWidenedLikeTheCpuSolver)
{
    // Blood Burst の形 (半径 0.06 に 650 粒)。そのまま出すと静止密度の何十倍にも詰まって爆ぜる。
    asset::FluidRecipe recipe = LiquidRecipe();
    recipe.liquid.particleRadius = 0.012f;
    const math::Vector3 center = { 0.0f, 0.0f, 0.0f };
    recipe.sources = { StillSphere(center, 0.06f, 650) };

    const auto spawns = asset::BuildGpuLiquidEmission(recipe, 1000);
    ASSERT_EQ(spawns.size(), 650u);
    // 同時に中に居る粒 325 (= 650 / kEmitPacking) が直径の間隔で収まる半径 ≈ 0.06 × 1.706。
    float farthest = 0.0f;
    for (const auto& spawn : spawns) farthest = (std::max)(farthest, DistanceTo(spawn, center));
    EXPECT_GT(farthest, 0.08f);
    EXPECT_LE(farthest, 0.06f * 1.71f);
}

TEST(FluidGpuLiquidTest, KernelMatchesTheCpuSolver)
{
    asset::FluidRecipe recipe = LiquidRecipe();
    recipe.liquid.particleRadius = 0.015f;
    asset::FluidLiquidSolver solver;
    solver.Reset(recipe, /*volumetric=*/true);

    const asset::GpuLiquidKernel kernel = asset::MakeGpuLiquidKernel(recipe.liquid.particleRadius);
    EXPECT_EQ(kernel.radius, solver.ParticleRadius());
    EXPECT_NEAR(kernel.h, 0.06f, 1.0e-7f);
    EXPECT_NEAR(kernel.restDensity, solver.RestDensity(), solver.RestDensity() * 1.0e-5f);
    EXPECT_GT(kernel.relaxation, 0.0f);
    EXPECT_GT(kernel.tensileScale, 0.0f);
    EXPECT_LT(kernel.spikyGradient, 0.0f);

    EXPECT_EQ(asset::MakeGpuLiquidKernel(1.0f).radius, 0.1f);
    EXPECT_EQ(asset::MakeGpuLiquidKernel(0.0f).radius, 0.002f);
}

TEST(FluidGpuLiquidTest, GridCoversTheCpuBoundsWithCellsOfH)
{
    const asset::GpuLiquidGrid grid = asset::MakeGpuLiquidGrid(0.012f);
    EXPECT_NEAR(grid.cellSize, 0.048f, 1.0e-7f);
    EXPECT_EQ(grid.cellsX, 67);  // 3.2 / 0.048 = 66.7
    EXPECT_EQ(grid.cellsY, 96);  // 4.6 / 0.048 = 95.8
    EXPECT_EQ(grid.cellsZ, 67);
    EXPECT_EQ(grid.boundsMin[1], -1.6f);
    EXPECT_EQ(grid.boundsMax[1], 3.0f);
    EXPECT_EQ(grid.boundsMax[0], 1.6f);

    // 粒子半径は核と同じ丸め方をする。
    EXPECT_NEAR(asset::MakeGpuLiquidGrid(5.0f).cellSize, 0.4f, 1.0e-7f);
}

TEST(FluidGpuLiquidTest, ForcesPackOnlyWhileActive)
{
    asset::FluidRecipe recipe = LiquidRecipe();
    asset::FluidForce off;
    off.enabled = false;
    asset::FluidForce wind;
    wind.type = asset::FluidForceType::Wind;
    wind.direction = { 0.0f, 0.0f, 2.0f };
    wind.strength = 3.0f;
    wind.radius = 0.5f;
    wind.falloffPower = 1.5f;
    wind.startTime = 1.0f;
    wind.duration = 1.0f;
    recipe.forces = { off, wind };

    asset::FluidGpuForce forces[asset::kMaxFluidGpuForces]{};
    ASSERT_EQ(asset::PackGpuLiquidForces(recipe, 0.5f, forces), 1);
    EXPECT_EQ(forces[0].directionStrength[3], 0.0f);
    EXPECT_NEAR(forces[0].directionStrength[2], 1.0f, 1.0e-6f);
    EXPECT_EQ(forces[0].centerType[3], static_cast<float>(asset::FluidForceType::Wind));

    ASSERT_EQ(asset::PackGpuLiquidForces(recipe, 1.5f, forces), 1);
    EXPECT_EQ(forces[0].directionStrength[3], 3.0f);
    EXPECT_EQ(forces[0].params[0], 0.5f);
    EXPECT_EQ(forces[0].params[1], 1.5f);
    EXPECT_EQ(asset::PackGpuLiquidForces(recipe, 2.5f, forces), 1);
    EXPECT_EQ(forces[0].directionStrength[3], 0.0f);
}

TEST(FluidGpuLiquidTest, CollidersPackUnwidenedSizesAndFrictionKeep)
{
    asset::FluidRecipe recipe = LiquidRecipe();
    asset::FluidCollider ball;
    ball.shape = asset::FluidColliderShape::Sphere;
    ball.size = { 0.001f, 0.5f, 0.9f };
    ball.friction = 0.4f;
    asset::FluidCollider box;
    box.shape = asset::FluidColliderShape::Box;
    box.size = { 0.3f, 0.001f, 0.4f };
    asset::FluidCollider slope;
    slope.shape = asset::FluidColliderShape::Plane;
    slope.direction = { 3.0f, 0.0f, 4.0f };
    slope.startTime = 1.0f;
    recipe.colliders = { ball, box, slope };

    asset::FluidGpuCollider colliders[asset::kMaxFluidGpuColliders]{};
    ASSERT_EQ(asset::PackGpuLiquidColliders(recipe, 0.0f, 0.01f, colliders), 3);
    // 液体は minSize = 0 で測る (気体のようにセル幅まで広げない)。
    EXPECT_EQ(colliders[0].sizeActive[0], 0.001f);
    EXPECT_EQ(colliders[0].sizeActive[1], 0.001f);
    EXPECT_EQ(colliders[0].sizeActive[2], 0.001f);
    EXPECT_EQ(colliders[0].sizeActive[3], 1.0f);
    EXPECT_NEAR(colliders[0].velocity[3], std::exp(-0.04f), 1.0e-6f);
    EXPECT_EQ(colliders[1].sizeActive[0], 0.3f);
    EXPECT_EQ(colliders[1].sizeActive[1], 0.001f);
    EXPECT_EQ(colliders[1].sizeActive[2], 0.4f);
    EXPECT_NEAR(colliders[2].normal[0], 0.6f, 1.0e-6f);
    EXPECT_NEAR(colliders[2].normal[2], 0.8f, 1.0e-6f);
    EXPECT_EQ(colliders[2].sizeActive[3], 0.0f);
    ASSERT_EQ(asset::PackGpuLiquidColliders(recipe, 1.5f, 0.01f, colliders), 3);
    EXPECT_EQ(colliders[2].sizeActive[3], 1.0f);
}

TEST(FluidGpuLiquidTest, SortCountIsAPowerOfTwoOfAtLeastOneBlock)
{
    EXPECT_EQ(asset::GpuLiquidSortCount(0), asset::kGpuLiquidSortBlock);
    EXPECT_EQ(asset::GpuLiquidSortCount(1), 256u);
    EXPECT_EQ(asset::GpuLiquidSortCount(256), 256u);
    EXPECT_EQ(asset::GpuLiquidSortCount(257), 512u);
    EXPECT_EQ(asset::GpuLiquidSortCount(asset::FluidGpuLiquidSolver::kMaxParticles), 32768u);
}

TEST(FluidGpuLiquidTest, SortStagesSwitchToTheLocalPassInsideOneBlock)
{
    const auto one = asset::BuildGpuLiquidSortStages(256);
    ASSERT_EQ(one.size(), 8u);
    std::uint32_t k = 2;
    for (const auto& stage : one) {
        EXPECT_TRUE(stage.local);
        EXPECT_EQ(stage.k, k);
        EXPECT_EQ(stage.j, k / 2u);
        k <<= 1;
    }

    const auto two = asset::BuildGpuLiquidSortStages(512);
    ASSERT_EQ(two.size(), 10u);
    EXPECT_EQ(two[8].k, 512u);
    EXPECT_EQ(two[8].j, 256u);
    EXPECT_FALSE(two[8].local);
    EXPECT_EQ(two[9].k, 512u);
    EXPECT_EQ(two[9].j, 128u);
    EXPECT_TRUE(two[9].local);

    const auto full = asset::BuildGpuLiquidSortStages(32768);
    ASSERT_FALSE(full.empty());
    EXPECT_EQ(full.back().k, 32768u);
    EXPECT_TRUE(full.back().local);
}

} // namespace fbzz::tests
