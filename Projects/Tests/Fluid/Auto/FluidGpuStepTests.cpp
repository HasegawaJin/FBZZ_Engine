/// @file    FluidGpuStepTests.cpp
/// @brief   GPU 流体ソルバーの刻みの定数 (CPU ソルバーと同じ規則か) を固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// GPU の結果は読み戻さないので、発生源の «今は出ているか» や散逸の換算がずれても絵でしか分からない。

#include <TestKit/TestKit.hpp>

#include <Fluid/FluidGpuStep.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace fbzz::tests {

TEST(FluidGpuStepTest, SourcesTurnOnAndOffWithTheirWindow)
{
    fluid::FluidRecipe recipe;
    fluid::FluidSource burst;
    burst.startTime = 0.5f;
    burst.duration = 0.25f;
    fluid::FluidSource forever;
    forever.startTime = 0.0f;
    forever.duration = 0.0f;
    recipe.sources = { burst, forever };

    EXPECT_EQ(fluid::PackFluidGpuStep(recipe, 64, 0.4f, 0.01f, 1.0f, 1.0f).sources[0].amounts[3], 0.0f);
    EXPECT_EQ(fluid::PackFluidGpuStep(recipe, 64, 0.6f, 0.01f, 1.0f, 1.0f).sources[0].amounts[3], 1.0f);
    EXPECT_EQ(fluid::PackFluidGpuStep(recipe, 64, 0.75f, 0.01f, 1.0f, 1.0f).sources[0].amounts[3], 0.0f);
    EXPECT_EQ(fluid::PackFluidGpuStep(recipe, 64, 9.0f, 0.01f, 1.0f, 1.0f).sources[1].amounts[3], 1.0f);
}

TEST(FluidGpuStepTest, DissipationIsConvertedToTheStepLength)
{
    fluid::FluidRecipe recipe;
    recipe.gas.densityDissipation = 0.5f;
    const auto step = fluid::PackFluidGpuStep(recipe, 32, 0.0f, 0.1f, 1.0f, 1.0f);
    EXPECT_NEAR(step.densityKeep, std::exp(-0.05f), 1.0e-6f);
    EXPECT_NEAR(step.cellSize, 2.0f / 32.0f, 1.0e-7f);
}

TEST(FluidGpuStepTest, TinySourcesGrowToOneCell)
{
    fluid::FluidRecipe recipe;
    fluid::FluidSource dot;
    dot.size = { 0.001f, 0.001f, 0.001f };
    recipe.sources = { dot };
    const auto step = fluid::PackFluidGpuStep(recipe, 16, 0.0f, 0.01f, 1.0f, 1.0f);
    EXPECT_NEAR(step.sources[0].sizeNoise[0], step.cellSize, 1.0e-7f);
    EXPECT_NEAR(step.sources[0].sizeNoise[2], step.cellSize, 1.0e-7f);
}

TEST(FluidGpuStepTest, SphereRadiusIsReplicatedOnEveryAxis)
{
    fluid::FluidRecipe recipe;
    fluid::FluidSource ball;
    ball.shape = fluid::FluidSourceShape::Sphere;
    ball.size = { 0.3f, 0.01f, 0.9f };
    recipe.sources = { ball };
    const auto step = fluid::PackFluidGpuStep(recipe, 16, 0.0f, 0.01f, 1.0f, 1.0f);
    EXPECT_EQ(step.sources[0].centerShape[3], 0.0f);
    EXPECT_EQ(step.sources[0].sizeNoise[0], 0.3f);
    EXPECT_EQ(step.sources[0].sizeNoise[1], 0.3f);
    EXPECT_EQ(step.sources[0].sizeNoise[2], 0.3f);
}

TEST(FluidGpuStepTest, MovingSourcePacksThePosedCenterAndItsSpeed)
{
    fluid::FluidRecipe recipe;
    fluid::FluidSource mover;
    mover.center = { 0.0f, -0.5f, 0.0f };
    mover.velocity = { 0.0f, 1.0f, 0.0f };
    mover.motion.keys = { fluid::FluidMotionKey{ 0.0f, { 0.0f, 0.0f, 0.0f } },
                          fluid::FluidMotionKey{ 1.0f, { 0.8f, 0.0f, 0.0f } } };
    mover.motion.inheritVelocity = true;
    recipe.sources = { mover };

    const auto step = fluid::PackFluidGpuStep(recipe, 32, 0.5f, 0.01f, 1.0f, 1.0f);
    const fluid::FluidGpuSource& s = step.sources[0];
    EXPECT_NEAR(s.centerShape[0], 0.4f, 1.0e-6f);
    EXPECT_NEAR(s.centerShape[1], -0.5f, 1.0e-6f);
    EXPECT_NEAR(s.velocity[0], 0.8f, 1.0e-5f);
    EXPECT_NEAR(s.velocity[1], 1.0f, 1.0e-6f);
    EXPECT_EQ(s.velocity[3], 1.0f);

    /// @note 動きの速度だけでも流速を与える (発生源が周りを引きずる)。
    recipe.sources[0].velocity = { 0.0f, 0.0f, 0.0f };
    const auto dragged = fluid::PackFluidGpuStep(recipe, 32, 0.5f, 0.01f, 1.0f, 1.0f);
    EXPECT_EQ(dragged.sources[0].velocity[3], 1.0f);

    /// @note 継がない設定なら流速は基準のまま。
    recipe.sources[0].motion.inheritVelocity = false;
    const auto still = fluid::PackFluidGpuStep(recipe, 32, 0.5f, 0.01f, 1.0f, 1.0f);
    EXPECT_NEAR(still.sources[0].centerShape[0], 0.4f, 1.0e-6f);
    EXPECT_EQ(still.sources[0].velocity[0], 0.0f);
    EXPECT_EQ(still.sources[0].velocity[3], 0.0f);
}

TEST(FluidGpuStepTest, DisabledPartsAreSkipped)
{
    fluid::FluidRecipe recipe;
    fluid::FluidSource off;
    off.enabled = false;
    off.center = { 0.9f, 0.9f, 0.9f };
    fluid::FluidSource on;
    on.center = { -0.3f, 0.0f, 0.0f };
    recipe.sources = { off, on };
    fluid::FluidForce offForce;
    offForce.enabled = false;
    fluid::FluidForce onForce;
    onForce.type = fluid::FluidForceType::Drag;
    recipe.forces = { offForce, onForce };

    const auto step = fluid::PackFluidGpuStep(recipe, 16, 0.0f, 0.01f, 1.0f, 1.0f);
    EXPECT_EQ(step.sourceCount, 1u);
    EXPECT_EQ(step.sources[0].centerShape[0], -0.3f);
    EXPECT_EQ(step.forceCount, 1u);
    EXPECT_EQ(step.forces[0].centerType[3], static_cast<float>(fluid::FluidForceType::Drag));
}

TEST(FluidGpuStepTest, ColorKeyPacksIntoExtraAndFollowsTheEnabledSources)
{
    fluid::FluidRecipe recipe;
    fluid::FluidSource warm;
    warm.colorKey = 0.25f;
    fluid::FluidSource hidden;
    hidden.enabled = false;
    hidden.colorKey = 0.5f;
    fluid::FluidSource cold;
    cold.colorKey = 0.75f;
    recipe.sources = { warm, hidden, cold };

    const auto step = fluid::PackFluidGpuStep(recipe, 16, 0.0f, 0.01f, 1.0f, 1.0f);
    ASSERT_EQ(step.sourceCount, 2u);
    EXPECT_EQ(step.sources[0].extra[0], 0.25f);
    /// @note 無効な発生源は詰めないので、後ろの鍵が前へ詰まる (GPU は詰めた順でしか発生源を知らない)。
    EXPECT_EQ(step.sources[1].extra[0], 0.75f);
    EXPECT_EQ(step.sources[1].extra[1], 0.0f);
    EXPECT_EQ(step.sources[1].extra[2], 0.0f);
    EXPECT_EQ(step.sources[1].extra[3], 0.0f);
    EXPECT_EQ(step.sources[2].extra[0], 0.0f);
}

TEST(FluidGpuStepTest, ConeAndRingPackTheirShapeAndANormalizedAxis)
{
    fluid::FluidRecipe recipe;
    fluid::FluidSource cone;
    cone.shape = fluid::FluidSourceShape::Cone;
    cone.size = { 0.2f, 0.6f, 0.0f };
    cone.direction = { 3.0f, 0.0f, 4.0f };
    fluid::FluidSource ring;
    ring.shape = fluid::FluidSourceShape::Ring;
    ring.size = { 0.5f, 0.1f, 0.0f };
    ring.direction = { 0.0f, 0.0f, 0.0f };
    recipe.sources = { cone, ring };

    const auto step = fluid::PackFluidGpuStep(recipe, 32, 0.0f, 0.01f, 1.0f, 1.0f);
    EXPECT_EQ(step.sources[0].centerShape[3], 2.0f);
    EXPECT_NEAR(step.sources[0].axis[0], 0.6f, 1.0e-6f);
    EXPECT_NEAR(step.sources[0].axis[1], 0.0f, 1.0e-6f);
    EXPECT_NEAR(step.sources[0].axis[2], 0.8f, 1.0e-6f);
    EXPECT_EQ(step.sources[0].sizeNoise[0], 0.2f);
    EXPECT_EQ(step.sources[0].sizeNoise[1], 0.6f);

    /// @note 長さ 0 の向きは上向き。
    EXPECT_EQ(step.sources[1].centerShape[3], 3.0f);
    EXPECT_EQ(step.sources[1].axis[0], 0.0f);
    EXPECT_EQ(step.sources[1].axis[1], 1.0f);
    EXPECT_EQ(step.sources[1].axis[2], 0.0f);
    EXPECT_EQ(step.sources[1].sizeNoise[1], 0.1f);
}

TEST(FluidGpuStepTest, ForcesPackTypeStrengthAndParams)
{
    fluid::FluidRecipe recipe;
    fluid::FluidForce vortex;
    vortex.type = fluid::FluidForceType::Vortex;
    vortex.center = { 0.1f, 0.2f, 0.3f };
    vortex.direction = { 0.0f, 2.0f, 0.0f };
    vortex.strength = 5.0f;
    vortex.radius = 0.7f;
    vortex.falloffPower = 1.5f;
    vortex.noiseFrequency = 4.0f;
    vortex.noiseSpeed = 0.25f;
    vortex.startTime = 1.0f;
    vortex.duration = 0.5f;
    fluid::FluidForce wind;
    wind.direction = { 0.0f, 0.0f, 0.0f };
    recipe.forces = { vortex, wind };

    const auto inside = fluid::PackFluidGpuStep(recipe, 32, 1.2f, 0.01f, 1.0f, 1.0f);
    ASSERT_EQ(inside.forceCount, 2u);
    const fluid::FluidGpuForce& f = inside.forces[0];
    EXPECT_EQ(f.centerType[0], 0.1f);
    EXPECT_EQ(f.centerType[2], 0.3f);
    EXPECT_EQ(f.centerType[3], static_cast<float>(fluid::FluidForceType::Vortex));
    EXPECT_NEAR(f.directionStrength[1], 1.0f, 1.0e-6f);
    EXPECT_EQ(f.directionStrength[3], 5.0f);
    EXPECT_EQ(f.params[0], 0.7f);
    EXPECT_EQ(f.params[1], 1.5f);
    EXPECT_EQ(f.params[2], 4.0f);
    EXPECT_EQ(f.params[3], 0.25f);
    /// @note 長さ 0 の向きは +X。
    EXPECT_EQ(inside.forces[1].directionStrength[0], 1.0f);
    EXPECT_EQ(inside.forces[1].directionStrength[1], 0.0f);

    /// @note 窓の外では強さ 0 で送る (枠は残る)。
    EXPECT_EQ(fluid::PackFluidGpuStep(recipe, 32, 0.9f, 0.01f, 1.0f, 1.0f).forces[0].directionStrength[3], 0.0f);
    EXPECT_EQ(fluid::PackFluidGpuStep(recipe, 32, 1.5f, 0.01f, 1.0f, 1.0f).forces[0].directionStrength[3], 0.0f);
    EXPECT_EQ(fluid::PackFluidGpuStep(recipe, 32, 1.5f, 0.01f, 1.0f, 1.0f).forceCount, 2u);
}

TEST(FluidGpuStepTest, MovingForcePacksThePosedCenter)
{
    fluid::FluidRecipe recipe;
    fluid::FluidForce force;
    force.type = fluid::FluidForceType::Attract;
    force.center = { 0.0f, 0.0f, 0.0f };
    force.motion.keys = { fluid::FluidMotionKey{ 0.0f, { 0.0f, 0.0f, 0.0f } },
                          fluid::FluidMotionKey{ 2.0f, { 0.0f, 1.0f, 0.0f } } };
    recipe.forces = { force };
    const auto step = fluid::PackFluidGpuStep(recipe, 32, 1.0f, 0.01f, 1.0f, 1.0f);
    EXPECT_NEAR(step.forces[0].centerType[1], 0.5f, 1.0e-6f);
}

TEST(FluidGpuStepTest, ExtraPartsAreDropped)
{
    fluid::FluidRecipe recipe;
    recipe.sources.resize(fluid::kMaxFluidGpuSources + 5);
    recipe.forces.resize(fluid::kMaxFluidGpuForces + 3);
    const auto step = fluid::PackFluidGpuStep(recipe, 16, 0.0f, 0.01f, 1.0f, 1.0f);
    EXPECT_EQ(step.sourceCount, static_cast<std::uint32_t>(fluid::kMaxFluidGpuSources));
    EXPECT_EQ(step.forceCount, static_cast<std::uint32_t>(fluid::kMaxFluidGpuForces));
}

TEST(FluidGpuStepTest, CollidersPackShapeWidenedSizeAndNormal)
{
    fluid::FluidRecipe recipe;
    fluid::FluidCollider ball;
    ball.shape = fluid::FluidColliderShape::Sphere;
    ball.center = { 0.1f, 0.2f, 0.3f };
    ball.size = { 0.001f, 0.5f, 0.9f };
    fluid::FluidCollider box;
    box.shape = fluid::FluidColliderShape::Box;
    box.size = { 0.3f, 0.001f, 0.4f };
    fluid::FluidCollider slope;
    slope.shape = fluid::FluidColliderShape::Plane;
    slope.direction = { 3.0f, 0.0f, 4.0f };
    fluid::FluidCollider flat;
    flat.shape = fluid::FluidColliderShape::Plane;
    flat.direction = { 0.0f, 0.0f, 0.0f };
    recipe.colliders = { ball, box, slope, flat };

    const auto step = fluid::PackFluidGpuStep(recipe, 16, 0.0f, 0.01f, 1.0f, 1.0f);
    ASSERT_EQ(step.colliderCount, 4u);

    /// @note 球は半径をセル幅まで広げて 3 軸に複製する (y・z の値は使わない)。
    const fluid::FluidGpuCollider& s = step.colliders[0];
    EXPECT_EQ(s.centerShape[0], 0.1f);
    EXPECT_EQ(s.centerShape[2], 0.3f);
    EXPECT_EQ(s.centerShape[3], 0.0f);
    EXPECT_NEAR(s.sizeActive[0], step.cellSize, 1.0e-7f);
    EXPECT_NEAR(s.sizeActive[1], step.cellSize, 1.0e-7f);
    EXPECT_NEAR(s.sizeActive[2], step.cellSize, 1.0e-7f);
    EXPECT_EQ(s.sizeActive[3], 1.0f);

    /// @note 箱は軸ごとに広げる。
    const fluid::FluidGpuCollider& b = step.colliders[1];
    EXPECT_EQ(b.centerShape[3], 1.0f);
    EXPECT_EQ(b.sizeActive[0], 0.3f);
    EXPECT_NEAR(b.sizeActive[1], step.cellSize, 1.0e-7f);
    EXPECT_EQ(b.sizeActive[2], 0.4f);

    EXPECT_EQ(step.colliders[2].centerShape[3], 2.0f);
    EXPECT_NEAR(step.colliders[2].normal[0], 0.6f, 1.0e-6f);
    EXPECT_NEAR(step.colliders[2].normal[1], 0.0f, 1.0e-6f);
    EXPECT_NEAR(step.colliders[2].normal[2], 0.8f, 1.0e-6f);
    /// @note 長さ 0 の法線は上向き。
    EXPECT_EQ(step.colliders[3].normal[0], 0.0f);
    EXPECT_EQ(step.colliders[3].normal[1], 1.0f);
    EXPECT_EQ(step.colliders[3].normal[2], 0.0f);
}

TEST(FluidGpuStepTest, CollidersComeAndGoWithTheirWindow)
{
    fluid::FluidRecipe recipe;
    fluid::FluidCollider gate;
    gate.startTime = 0.5f;
    gate.duration = 0.25f;
    recipe.colliders = { gate };

    EXPECT_EQ(fluid::PackFluidGpuStep(recipe, 32, 0.4f, 0.01f, 1.0f, 1.0f).colliders[0].sizeActive[3], 0.0f);
    EXPECT_EQ(fluid::PackFluidGpuStep(recipe, 32, 0.6f, 0.01f, 1.0f, 1.0f).colliders[0].sizeActive[3], 1.0f);
    EXPECT_EQ(fluid::PackFluidGpuStep(recipe, 32, 0.75f, 0.01f, 1.0f, 1.0f).colliders[0].sizeActive[3], 0.0f);
    /// @note 居ない刻みも枠は残す (GPU 側は w で読み飛ばす)。
    EXPECT_EQ(fluid::PackFluidGpuStep(recipe, 32, 0.9f, 0.01f, 1.0f, 1.0f).colliderCount, 1u);
}

TEST(FluidGpuStepTest, MovingColliderPacksThePosedCenterAndItsSpeed)
{
    fluid::FluidRecipe recipe;
    fluid::FluidCollider mover;
    mover.center = { 0.0f, -0.5f, 0.0f };
    mover.motion.keys = { fluid::FluidMotionKey{ 0.0f, { 0.0f, 0.0f, 0.0f } },
                          fluid::FluidMotionKey{ 1.0f, { 0.8f, 0.0f, 0.0f } } };
    mover.motion.inheritVelocity = true;
    recipe.colliders = { mover };

    const auto step = fluid::PackFluidGpuStep(recipe, 32, 0.5f, 0.01f, 1.0f, 1.0f);
    const fluid::FluidGpuCollider& c = step.colliders[0];
    EXPECT_NEAR(c.centerShape[0], 0.4f, 1.0e-6f);
    EXPECT_NEAR(c.centerShape[1], -0.5f, 1.0e-6f);
    EXPECT_NEAR(c.velocity[0], 0.8f, 1.0e-5f);
    EXPECT_EQ(c.velocity[1], 0.0f);
    EXPECT_EQ(c.velocity[3], 0.0f);

    /// @note 継がない設定なら動いても流れは押さない (位置だけ動く)。
    recipe.colliders[0].motion.inheritVelocity = false;
    const auto still = fluid::PackFluidGpuStep(recipe, 32, 0.5f, 0.01f, 1.0f, 1.0f);
    EXPECT_NEAR(still.colliders[0].centerShape[0], 0.4f, 1.0e-6f);
    EXPECT_EQ(still.colliders[0].velocity[0], 0.0f);
    EXPECT_EQ(still.colliders[0].velocity[1], 0.0f);
    EXPECT_EQ(still.colliders[0].velocity[2], 0.0f);
}

TEST(FluidGpuStepTest, DisabledAndExtraCollidersAreDropped)
{
    fluid::FluidRecipe recipe;
    EXPECT_EQ(fluid::PackFluidGpuStep(recipe, 16, 0.0f, 0.01f, 1.0f, 1.0f).colliderCount, 0u);

    fluid::FluidCollider off;
    off.enabled = false;
    off.center = { 0.9f, 0.9f, 0.9f };
    fluid::FluidCollider on;
    on.center = { -0.3f, 0.0f, 0.0f };
    recipe.colliders = { off, on };
    const auto step = fluid::PackFluidGpuStep(recipe, 16, 0.0f, 0.01f, 1.0f, 1.0f);
    EXPECT_EQ(step.colliderCount, 1u);
    EXPECT_EQ(step.colliders[0].centerShape[0], -0.3f);

    recipe.colliders.assign(fluid::kMaxFluidGpuColliders + 3, on);
    EXPECT_EQ(fluid::PackFluidGpuStep(recipe, 16, 0.0f, 0.01f, 1.0f, 1.0f).colliderCount,
              static_cast<std::uint32_t>(fluid::kMaxFluidGpuColliders));
}

TEST(FluidGpuStepTest, TextureSourcesTakeAtlasTilesInPackingOrder)
{
    fluid::FluidRecipe recipe;
    fluid::FluidSource ball;
    ball.shape = fluid::FluidSourceShape::Sphere;
    fluid::FluidSource logo;
    logo.shape = fluid::FluidSourceShape::Texture;
    logo.texture = "Textures/Logo.png";
    fluid::FluidSource hidden = logo;
    hidden.enabled = false;
    hidden.texture = "Textures/Hidden.png";
    fluid::FluidSource cone;
    cone.shape = fluid::FluidSourceShape::Cone;
    fluid::FluidSource rune = logo;
    rune.texture = "Textures/Rune.png";
    recipe.sources = { ball, logo, hidden, cone, rune };

    const auto step = fluid::PackFluidGpuStep(recipe, 32, 0.0f, 0.01f, 1.0f, 1.0f);
    ASSERT_EQ(step.sourceCount, 4u);
    EXPECT_EQ(step.sources[0].axis[3], -1.0f);
    EXPECT_EQ(step.sources[1].centerShape[3], 4.0f);
    EXPECT_EQ(step.sources[1].axis[3], 0.0f);
    EXPECT_EQ(step.sources[2].axis[3], -1.0f);
    EXPECT_EQ(step.sources[3].axis[3], 1.0f);

    /// @note タイル番号と同じ順・同じ絞り込み (無効な発生源は数えない)。
    const std::vector<std::string> expected = { "Textures/Logo.png", "Textures/Rune.png" };
    EXPECT_EQ(fluid::FluidGpuMaskPaths(recipe), expected);
}

TEST(FluidGpuStepTest, TextureSourceAxisIsTheBasisNormal)
{
    fluid::FluidRecipe recipe;
    fluid::FluidSource tilted;
    tilted.shape = fluid::FluidSourceShape::Texture;
    tilted.direction = { 0.0f, 3.0f, 4.0f };
    fluid::FluidSource facing;
    facing.shape = fluid::FluidSourceShape::Texture;
    facing.direction = { 0.0f, 0.0f, 0.0f };
    recipe.sources = { tilted, facing };

    const auto step = fluid::PackFluidGpuStep(recipe, 32, 0.0f, 0.01f, 1.0f, 1.0f);
    EXPECT_NEAR(step.sources[0].axis[0], 0.0f, 1.0e-6f);
    EXPECT_NEAR(step.sources[0].axis[1], 0.6f, 1.0e-6f);
    EXPECT_NEAR(step.sources[0].axis[2], 0.8f, 1.0e-6f);
    /// @note 長さ 0 の向きは Cone / Ring (上向き) と違い、手前向き (画像が正面に見える)。
    EXPECT_NEAR(step.sources[1].axis[0], 0.0f, 1.0e-6f);
    EXPECT_NEAR(step.sources[1].axis[1], 0.0f, 1.0e-6f);
    EXPECT_NEAR(step.sources[1].axis[2], 1.0f, 1.0e-6f);
}

TEST(FluidGpuStepTest, MaskPathsStopAtTheSourceLimit)
{
    fluid::FluidRecipe recipe;
    recipe.sources.resize(fluid::kMaxFluidGpuSources);
    fluid::FluidSource late;
    late.shape = fluid::FluidSourceShape::Texture;
    late.texture = "Textures/Late.png";
    recipe.sources.push_back(late);
    /// @note 詰めない発生源のマスクはアトラスにも置かない (置くとタイル番号が 1 つずれる)。
    EXPECT_TRUE(fluid::FluidGpuMaskPaths(recipe).empty());

    recipe.sources[3] = late;
    const std::vector<std::string> paths = fluid::FluidGpuMaskPaths(recipe);
    ASSERT_EQ(paths.size(), 1u);
    EXPECT_EQ(paths[0], "Textures/Late.png");
    EXPECT_EQ(fluid::PackFluidGpuStep(recipe, 16, 0.0f, 0.01f, 1.0f, 1.0f).sources[3].axis[3], 0.0f);
}

TEST(FluidGpuStepTest, NoiseOffsetDependsOnlyOnTheSeed)
{
    const math::Vector3 a = fluid::FluidNoiseOffset(7);
    const math::Vector3 b = fluid::FluidNoiseOffset(7);
    const math::Vector3 c = fluid::FluidNoiseOffset(8);
    EXPECT_EQ(a.x, b.x);
    EXPECT_EQ(a.z, b.z);
    EXPECT_TRUE(a.x != c.x || a.y != c.y || a.z != c.z);
}

} // namespace fbzz::tests
