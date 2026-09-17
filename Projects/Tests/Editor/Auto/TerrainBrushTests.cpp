/// @file    TerrainBrushTests.cpp
/// @brief   地形ブラシの減衰カーブ・座標変換と、塗り / 彫刻 / 坂 / 穴のカーネルの契約。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// @note 減衰や座標変換の狂いは «なんとなく変» としか言えない壊れ方をするので数値で固定する。
/// @note 人 (TerrainTool) と AI バスが同じカーネルを叩くため、侵食の決定性もここで固定する。
/// @see Docs/design/terrain-layers.md
#include <TestKit/TestKit.hpp>
#include <TestKit/Fixture.hpp>

#include <Tools/TerrainBrush.hpp>

#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/TerrainSplat.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace fbzz::tests {
namespace {

using editor::TerrainBrush;
using editor::TerrainBrushWeight;
using editor::TerrainFalloff;

TerrainBrush MakeBrush(float radius, TerrainFalloff falloff)
{
    TerrainBrush brush;
    brush.radius  = radius;
    brush.falloff = falloff;
    return brush;
}

constexpr float kEps = 1e-4f;

} // namespace

/// @name 減衰カーブ

TEST(TerrainBrushWeightTest, IsFullStrengthAtTheCentre)
{
    for (const auto falloff : { TerrainFalloff::Linear, TerrainFalloff::Smooth,
                                TerrainFalloff::Gaussian }) {
        EXPECT_NEAR(TerrainBrushWeight(MakeBrush(5.0f, falloff), 0.0f), 1.0f, kEps);
    }
}

TEST(TerrainBrushWeightTest, IsZeroAtAndBeyondTheEdge)
{
    /// @note 縁で 0 にならないと、塗った範囲の外周に段差が残る。
    for (const auto falloff : { TerrainFalloff::Linear, TerrainFalloff::Smooth,
                                TerrainFalloff::Gaussian }) {
        const TerrainBrush brush = MakeBrush(5.0f, falloff);
        EXPECT_FLOAT_EQ(TerrainBrushWeight(brush, 5.0f), 0.0f);
        EXPECT_FLOAT_EQ(TerrainBrushWeight(brush, 5.1f), 0.0f);
        EXPECT_FLOAT_EQ(TerrainBrushWeight(brush, 100.0f), 0.0f);
    }
}

TEST(TerrainBrushWeightTest, DecreasesMonotonicallyOutward)
{
    for (const auto falloff : { TerrainFalloff::Linear, TerrainFalloff::Smooth,
                                TerrainFalloff::Gaussian }) {
        const TerrainBrush brush = MakeBrush(10.0f, falloff);
        float previous = TerrainBrushWeight(brush, 0.0f);
        for (float dist = 0.5f; dist < 10.0f; dist += 0.5f) {
            const float current = TerrainBrushWeight(brush, dist);
            EXPECT_LE(current, previous) << "dist=" << dist;
            previous = current;
        }
    }
}

TEST(TerrainBrushWeightTest, LinearFallsOffProportionally)
{
    const TerrainBrush brush = MakeBrush(10.0f, TerrainFalloff::Linear);

    EXPECT_NEAR(TerrainBrushWeight(brush, 2.5f), 0.75f, kEps);
    EXPECT_NEAR(TerrainBrushWeight(brush, 5.0f), 0.5f,  kEps);
    EXPECT_NEAR(TerrainBrushWeight(brush, 7.5f), 0.25f, kEps);
}

TEST(TerrainBrushWeightTest, SmoothIsFlatAtBothEnds)
{
    /// @note smoothstep の要点は端の傾きが 0 になること。中心と縁で «急に変わらない»。
    const TerrainBrush brush = MakeBrush(10.0f, TerrainFalloff::Smooth);

    /// @note 中間は 0.5
    EXPECT_NEAR(TerrainBrushWeight(brush, 5.0f), 0.5f, kEps);

    /// @note 中心付近の落ち込みは線形より緩い。
    EXPECT_GT(TerrainBrushWeight(brush, 1.0f),
              TerrainBrushWeight(MakeBrush(10.0f, TerrainFalloff::Linear), 1.0f));
}

TEST(TerrainBrushWeightTest, GaussianKeepsAThinTailAtTheEdge)
{
    /// @note exp(-3) ≒ 0.0498。縁を «0 にする» 側の打ち切りが効いていることも同時に見る。
    const TerrainBrush brush = MakeBrush(10.0f, TerrainFalloff::Gaussian);

    EXPECT_NEAR(TerrainBrushWeight(brush, 9.99f), 0.0498f, 1e-3f);
    EXPECT_FLOAT_EQ(TerrainBrushWeight(brush, 10.0f), 0.0f);
}

TEST(TerrainBrushWeightTest, ScalesWithTheRadius)
{
    /// @note 同じ «相対位置» なら半径が変わっても重みは同じ。
    const float small = TerrainBrushWeight(MakeBrush(2.0f,  TerrainFalloff::Linear), 1.0f);
    const float large = TerrainBrushWeight(MakeBrush(20.0f, TerrainFalloff::Linear), 10.0f);

    EXPECT_NEAR(small, large, kEps);
}

TEST(TerrainBrushWeightTest, IsZeroForADegenerateRadius)
{
    /// @note 半径 0 で 1.0 を返すと、1 点だけ無限に盛り上がる。
    EXPECT_FLOAT_EQ(TerrainBrushWeight(MakeBrush(0.0f, TerrainFalloff::Linear), 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(TerrainBrushWeight(MakeBrush(-1.0f, TerrainFalloff::Smooth), 0.0f), 0.0f);
}

TEST(TerrainBrushWeightTest, StaysWithinTheUnitRange)
{
    for (const auto falloff : { TerrainFalloff::Linear, TerrainFalloff::Smooth,
                                TerrainFalloff::Gaussian }) {
        const TerrainBrush brush = MakeBrush(8.0f, falloff);
        for (float dist = 0.0f; dist <= 8.0f; dist += 0.25f) {
            const float weight = TerrainBrushWeight(brush, dist);
            EXPECT_GE(weight, 0.0f) << "dist=" << dist;
            EXPECT_LE(weight, 1.0f) << "dist=" << dist;
        }
    }
}

/// @name 座標変換

TEST(TerrainTransform, IsIdentityForAnUntransformedTerrain)
{
    scene::Transform transform;

    const math::Vector3 local = editor::ToTerrainLocal(transform, { 3.0f, 4.0f, 5.0f });

    EXPECT_NEAR(local.x, 3.0f, kEps);
    EXPECT_NEAR(local.y, 4.0f, kEps);
    EXPECT_NEAR(local.z, 5.0f, kEps);
}

TEST(TerrainTransform, SubtractsTheTerrainPosition)
{
    scene::Transform transform;
    transform.worldPosition = { 10.0f, 0.0f, -5.0f };

    const math::Vector3 local = editor::ToTerrainLocal(transform, { 12.0f, 1.0f, -3.0f });

    EXPECT_NEAR(local.x, 2.0f, kEps);
    EXPECT_NEAR(local.y, 1.0f, kEps);
    EXPECT_NEAR(local.z, 2.0f, kEps);
}

TEST(TerrainTransform, RoundTripsThroughLocalAndBack)
{
    /// @note 片道でもずれると «クリックした場所と盛り上がる場所» が食い違う。
    scene::Transform transform;
    transform.worldPosition = { 7.0f, 2.0f, -3.0f };
    transform.worldRotation =
        math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(45.0f));
    transform.worldScale    = { 2.0f, 1.0f, 2.0f };

    const math::Vector3 world{ 11.0f, 5.0f, 4.0f };
    const math::Vector3 back =
        editor::ToTerrainWorld(transform, editor::ToTerrainLocal(transform, world));

    EXPECT_NEAR(back.x, world.x, 1e-3f);
    EXPECT_NEAR(back.y, world.y, 1e-3f);
    EXPECT_NEAR(back.z, world.z, 1e-3f);
}

TEST(TerrainTransform, RoundTripsWithScaleOnly)
{
    scene::Transform transform;
    transform.worldScale = { 4.0f, 1.0f, 4.0f };

    const math::Vector3 world{ 8.0f, 0.0f, 12.0f };
    const math::Vector3 local = editor::ToTerrainLocal(transform, world);

    EXPECT_NEAR(local.x, 2.0f, kEps);
    EXPECT_NEAR(local.z, 3.0f, kEps);

    const math::Vector3 back = editor::ToTerrainWorld(transform, local);
    EXPECT_NEAR(back.x, world.x, 1e-3f);
    EXPECT_NEAR(back.z, world.z, 1e-3f);
}

/// @name カーネル (塗り / 彫刻 / 坂 / 穴)

namespace {

/// @brief 1 m 格子・maxHeight 10 m の平坦な地形。
scene::TerrainComponent MakeFlatTerrain(int columns, int rows)
{
    scene::TerrainComponent terrain;
    terrain.columns   = columns;
    terrain.rows      = rows;
    terrain.cellSize  = 1.0f;
    terrain.maxHeight = 10.0f;
    terrain.InitFlat(0.0f);
    return terrain;
}

float& HeightAt(scene::TerrainComponent& terrain, int x, int z)
{
    return terrain.heightData[static_cast<size_t>(z) * static_cast<size_t>(terrain.columns)
                            + static_cast<size_t>(x)];
}

/// @return 8 近傍との高低差 (正規化) の最大値。
float MaxNeighbourDifference(const scene::TerrainComponent& terrain)
{
    float maxDiff = 0.0f;
    for (int z = 0; z < terrain.rows; ++z)
        for (int x = 0; x < terrain.columns; ++x)
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx;
                    const int nz = z + dz;
                    if (nx < 0 || nz < 0 || nx >= terrain.columns || nz >= terrain.rows) continue;
                    const float a = terrain.heightData[static_cast<size_t>(z) * terrain.columns + x];
                    const float b = terrain.heightData[static_cast<size_t>(nz) * terrain.columns + nx];
                    maxDiff = std::max(maxDiff, std::abs(a - b));
                }
    return maxDiff;
}

class TerrainBrushKernelTest : public testkit::Fixture {};

} // namespace

TEST_F(TerrainBrushKernelTest, PaintReachesASixthLayerOnAVertexHoldingFourLayers)
{
    scene::TerrainComponent terrain = MakeFlatTerrain(17, 17);
    terrain.layerMaterials.resize(6);
    const size_t base = (8u * 17u + 8u) * scene::TERRAIN_SPLAT_SLOTS;
    const int   layers[]  = { 0, 1, 2, 3 };
    const float weights[] = { 1.0f, 1.0f, 1.0f, 1.0f };
    scene::terrain_splat::Canonicalize(layers, weights, 4, &terrain.splatIndices[base], &terrain.splatWeights[base]);
    editor::TerrainBrush brush = MakeBrush(2.0f, TerrainFalloff::Linear);
    brush.strength = 1.0f;

    editor::ApplyTerrainPaint(terrain, { 8.0f, 0.0f, 8.0f }, brush, 5, 1.0f);

    EXPECT_NEAR(terrain.GetLayerWeightAtGrid(8, 8, 5), 1.0f, 1.0f / 255.0f);
    EXPECT_NEAR(terrain.GetLayerWeightAtGrid(8, 8, 0), 0.0f, 1.0f / 255.0f);
}

TEST_F(TerrainBrushKernelTest, PaintIgnoresALayerBeyondTheLayerCount)
{
    scene::TerrainComponent terrain = MakeFlatTerrain(9, 9);
    const std::vector<std::uint8_t> before = terrain.splatWeights;
    editor::TerrainBrush brush = MakeBrush(3.0f, TerrainFalloff::Linear);
    brush.strength = 1.0f;

    editor::ApplyTerrainPaint(terrain, { 4.0f, 0.0f, 4.0f }, brush, terrain.LayerCount(), 1.0f);

    EXPECT_EQ(terrain.splatWeights, before);
}

TEST_F(TerrainBrushKernelTest, NoiseIsDeterministicAndStaysInsideTheRadius)
{
    scene::TerrainComponent a = MakeFlatTerrain(33, 33);
    scene::TerrainComponent b = MakeFlatTerrain(33, 33);
    editor::TerrainBrush brush = MakeBrush(6.0f, TerrainFalloff::Smooth);
    brush.strength   = 1.0f;
    brush.seed       = 7;
    brush.noiseScale = 4.0f;
    const math::Vector3 centre{ 16.0f, 0.0f, 16.0f };

    editor::ApplyTerrainSculpt(a, centre, brush, editor::TerrainSculptOp::Noise, 0.0f, 1.0f);
    editor::ApplyTerrainSculpt(b, centre, brush, editor::TerrainSculptOp::Noise, 0.0f, 1.0f);

    EXPECT_EQ(a.heightData, b.heightData);
    bool changedInside = false;
    for (int z = 0; z < a.rows; ++z) {
        for (int x = 0; x < a.columns; ++x) {
            const float dist = std::hypot(static_cast<float>(x) - centre.x, static_cast<float>(z) - centre.z);
            const float h = HeightAt(a, x, z);
            if (dist >= brush.radius)
                EXPECT_FLOAT_EQ(h, 0.0f) << "x=" << x << " z=" << z;
            else if (h != 0.0f)
                changedInside = true;
        }
    }
    EXPECT_TRUE(changedInside);
}

TEST_F(TerrainBrushKernelTest, TerraceWithFullSharpnessSnapsHeightsToSteps)
{
    scene::TerrainComponent terrain = MakeFlatTerrain(9, 9);
    constexpr float kStepMeters = 2.0f;
    for (int z = 0; z < terrain.rows; ++z)
        for (int x = 0; x < terrain.columns; ++x) {
            /// @note 段の中の位置 f は 0.4 以下に置く。f^9 の残りが段の高さの 0.03% 未満に収まる。
            const float fraction = 0.1f * static_cast<float>((x + z) % 5);
            HeightAt(terrain, x, z) = (static_cast<float>(x % 3) + fraction) * kStepMeters / terrain.maxHeight;
        }
    editor::TerrainBrush brush = MakeBrush(1.0e4f, TerrainFalloff::Linear);
    brush.strength         = 1.0f;
    brush.terraceStep      = kStepMeters;
    brush.terraceSharpness = 1.0f;

    editor::ApplyTerrainSculpt(terrain, { 4.0f, 0.0f, 4.0f }, brush, editor::TerrainSculptOp::Terrace, 0.0f, 1.0f);

    for (int z = 0; z < terrain.rows; ++z)
        for (int x = 0; x < terrain.columns; ++x) {
            const float expected = static_cast<float>(x % 3) * kStepMeters / terrain.maxHeight;
            EXPECT_NEAR(HeightAt(terrain, x, z), expected, 1.0e-3f) << "x=" << x << " z=" << z;
        }
}

TEST_F(TerrainBrushKernelTest, ThermalErosionReducesTheSteepestSlopeOfASpike)
{
    scene::TerrainComponent terrain = MakeFlatTerrain(17, 17);
    HeightAt(terrain, 8, 8) = 1.0f;
    float sumBefore = 0.0f;
    for (float h : terrain.heightData) sumBefore += h;
    const float slopeBefore = MaxNeighbourDifference(terrain);
    editor::TerrainBrush brush = MakeBrush(5.0f, TerrainFalloff::Linear);
    brush.strength     = 1.0f;
    brush.talusDegrees = 35.0f;

    for (int i = 0; i < 20; ++i)
        editor::ApplyTerrainSculpt(terrain, { 8.0f, 0.0f, 8.0f }, brush,
                                   editor::TerrainSculptOp::ThermalErosion, 0.0f, 1.0f);

    float sumAfter = 0.0f;
    for (float h : terrain.heightData) sumAfter += h;
    EXPECT_LT(MaxNeighbourDifference(terrain), slopeBefore * 0.5f);
    /// @note 崩した土は近傍へ移すだけなので、総量は保たれる。
    EXPECT_NEAR(sumAfter, sumBefore, testkit::kLooseTolerance);
}

TEST_F(TerrainBrushKernelTest, HydraulicErosionIsDeterministicAndStaysInRange)
{
    scene::TerrainComponent source = MakeFlatTerrain(33, 33);
    for (int z = 0; z < source.rows; ++z)
        for (int x = 0; x < source.columns; ++x)
            HeightAt(source, x, z) = 0.02f * static_cast<float>(x) - 0.3f
                                   + 0.05f * std::sin(static_cast<float>(z) * 0.7f);
    editor::TerrainBrush brush = MakeBrush(10.0f, TerrainFalloff::Smooth);
    brush.strength        = 1.0f;
    brush.seed            = 3;
    brush.erosionDroplets = 64;
    const math::Vector3 centre{ 16.0f, 0.0f, 16.0f };

    scene::TerrainComponent a = source;
    scene::TerrainComponent b = source;
    scene::TerrainComponent c = source;
    editor::ApplyTerrainSculpt(a, centre, brush, editor::TerrainSculptOp::HydraulicErosion, 0.0f, 1.0f, 5);
    editor::ApplyTerrainSculpt(b, centre, brush, editor::TerrainSculptOp::HydraulicErosion, 0.0f, 1.0f, 5);
    editor::ApplyTerrainSculpt(c, centre, brush, editor::TerrainSculptOp::HydraulicErosion, 0.0f, 1.0f, 6);

    EXPECT_EQ(a.heightData, b.heightData);
    EXPECT_NE(a.heightData, source.heightData);
    EXPECT_NE(a.heightData, c.heightData);
    for (float h : a.heightData) {
        EXPECT_GE(h, -1.0f);
        EXPECT_LE(h, 1.0f);
    }
}

TEST_F(TerrainBrushKernelTest, RampProducesAMonotonicProfileAlongTheSegment)
{
    scene::TerrainComponent terrain = MakeFlatTerrain(17, 17);
    editor::TerrainBrush brush = MakeBrush(2.0f, TerrainFalloff::Smooth);
    brush.strength = 1.0f;
    const math::Vector3 start{ 2.0f, 0.0f, 8.0f };
    const math::Vector3 end{ 14.0f, 5.0f, 8.0f };

    editor::ApplyTerrainRamp(terrain, start, end, brush);

    EXPECT_NEAR(HeightAt(terrain, 2, 8),  0.0f, testkit::kTolerance);
    EXPECT_NEAR(HeightAt(terrain, 14, 8), 0.5f, testkit::kTolerance);
    EXPECT_NEAR(HeightAt(terrain, 8, 8),  0.25f, testkit::kTolerance);
    for (int x = 3; x <= 14; ++x)
        EXPECT_GT(HeightAt(terrain, x, 8), HeightAt(terrain, x - 1, 8)) << "x=" << x;
    /// @note 線分から radius 以上離れた頂点は変えない。
    EXPECT_FLOAT_EQ(HeightAt(terrain, 8, 11), 0.0f);
}

TEST_F(TerrainBrushKernelTest, HoleBrushCutsCellsWhoseCentreIsInsideAndEraseFillsThem)
{
    scene::TerrainComponent terrain = MakeFlatTerrain(17, 17);
    const editor::TerrainBrush brush = MakeBrush(1.0f, TerrainFalloff::Linear);
    const math::Vector3 centre{ 8.0f, 0.0f, 8.0f };

    const bool cut = editor::ApplyTerrainHole(terrain, centre, brush, true);

    EXPECT_TRUE(cut);
    EXPECT_EQ(terrain.CountHoles(), 4u);
    EXPECT_TRUE(terrain.IsHoleCell(7, 7));
    EXPECT_TRUE(terrain.IsHoleCell(8, 7));
    EXPECT_TRUE(terrain.IsHoleCell(7, 8));
    EXPECT_TRUE(terrain.IsHoleCell(8, 8));
    EXPECT_FALSE(terrain.IsHoleCell(6, 7));
    EXPECT_FALSE(editor::ApplyTerrainHole(terrain, centre, brush, true));

    EXPECT_TRUE(editor::ApplyTerrainHole(terrain, centre, brush, false));
    EXPECT_EQ(terrain.CountHoles(), 0u);
}

} // namespace fbzz::tests
