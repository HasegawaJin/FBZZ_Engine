/// @file    TerrainBrushTests.cpp
/// @brief   地形ブラシの減衰カーブと、ワールド ⇄ 地形ローカルの変換。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// 減衰が端で 0 にならないと、ブラシの縁に段差が残って «塗った跡» が四角く見える。
/// 座標変換が片道でも狂うと、クリックした場所と盛り上がる場所がずれる。
/// どちらも «なんとなく変» としか言えない壊れ方をするので、数値で固定する。
#include <TestKit/TestKit.hpp>

#include <Tools/TerrainBrush.hpp>

#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>

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

// --- 減衰カーブ -------------------------------------------------------------

TEST(TerrainBrushWeightTest, IsFullStrengthAtTheCentre)
{
    for (const auto falloff : { TerrainFalloff::Linear, TerrainFalloff::Smooth,
                                TerrainFalloff::Gaussian }) {
        EXPECT_NEAR(TerrainBrushWeight(MakeBrush(5.0f, falloff), 0.0f), 1.0f, kEps);
    }
}

TEST(TerrainBrushWeightTest, IsZeroAtAndBeyondTheEdge)
{
    // 縁で 0 にならないと、塗った範囲の外周に段差が残る。
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
    // smoothstep の要点は端の傾きが 0 になること。中心と縁で «急に変わらない»。
    const TerrainBrush brush = MakeBrush(10.0f, TerrainFalloff::Smooth);

    EXPECT_NEAR(TerrainBrushWeight(brush, 5.0f), 0.5f, kEps);   // 中間は 0.5

    // 中心付近の落ち込みは線形より緩い。
    EXPECT_GT(TerrainBrushWeight(brush, 1.0f),
              TerrainBrushWeight(MakeBrush(10.0f, TerrainFalloff::Linear), 1.0f));
}

TEST(TerrainBrushWeightTest, GaussianKeepsAThinTailAtTheEdge)
{
    // exp(-3) ≒ 0.0498。縁を «0 にする» 側の打ち切りが効いていることも同時に見る。
    const TerrainBrush brush = MakeBrush(10.0f, TerrainFalloff::Gaussian);

    EXPECT_NEAR(TerrainBrushWeight(brush, 9.99f), 0.0498f, 1e-3f);
    EXPECT_FLOAT_EQ(TerrainBrushWeight(brush, 10.0f), 0.0f);
}

TEST(TerrainBrushWeightTest, ScalesWithTheRadius)
{
    // 同じ «相対位置» なら半径が変わっても重みは同じ。
    const float small = TerrainBrushWeight(MakeBrush(2.0f,  TerrainFalloff::Linear), 1.0f);
    const float large = TerrainBrushWeight(MakeBrush(20.0f, TerrainFalloff::Linear), 10.0f);

    EXPECT_NEAR(small, large, kEps);
}

TEST(TerrainBrushWeightTest, IsZeroForADegenerateRadius)
{
    // 半径 0 で 1.0 を返すと、1 点だけ無限に盛り上がる。
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

// --- 座標変換 ---------------------------------------------------------------

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
    // 片道でもずれると «クリックした場所と盛り上がる場所» が食い違う。
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

} // namespace fbzz::tests
