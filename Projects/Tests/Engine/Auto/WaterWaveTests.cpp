/// @file    WaterWaveTests.cpp
/// @brief   水面の実効波 (.mat × 倍率 × 環境風) の解決と、CPU 側の水面高さを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// 浮力・水中判定・スクリプトは、描画と同じ波を CPU で評価している。ここがずれると
/// «見えている水面より下で浮く» «潜っているのに水中エフェクトが出ない» という形で表に出る。
#include <TestKit/TestKit.hpp>

#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Systems/WaterSystem.hpp>

#include <Math/MathUtils.hpp>
#include <cmath>

namespace fbzz::tests {
namespace {

using namespace scene::water_keys;

/// 波 0 だけを持つ .mat。残りの波は振幅 0 で明示的に消す。
asset::MaterialAsset SingleWaveMaterial(math::Vector2 direction, float amplitude, float wavelength)
{
    asset::MaterialAsset mat;
    for (int i = 0; i < 4; ++i)
        mat.params[kWaveAmplitude[i]] = { 0.0f };
    mat.params[kWaveDirection[0]]  = { direction.x, direction.y };
    mat.params[kWaveAmplitude[0]]  = { amplitude };
    mat.params[kWaveWavelength[0]] = { wavelength };
    mat.params[kWaveSteepness[0]]  = { 0.5f };
    return mat;
}

} // namespace

// --- .mat と個体の補正 --------------------------------------------------------

TEST(WaterWaves, MaterialWithoutWaveKeysFallsBackToTheDefaultWaves)
{
    // 波を .mat へ移す前の .mat を開いた瞬間に、海が止まってはいけない。
    scene::WaterComponent water;
    const asset::MaterialAsset legacy{};
    scene::ResolveWaterWaves(water, &legacy, {});
    for (int i = 0; i < 4; ++i)
        EXPECT_FLOAT_EQ(water.waves[static_cast<size_t>(i)].amplitude,
                        scene::DefaultWaterWave(i).amplitude) << "wave " << i;
}

TEST(WaterWaves, AmplitudeScaleAndDisableApplyOnTopOfTheMaterial)
{
    const asset::MaterialAsset mat = SingleWaveMaterial({ 1.0f, 0.0f }, 0.4f, 10.0f);
    scene::WaterComponent water;

    water.waveAmplitudeScale = 0.5f;
    scene::ResolveWaterWaves(water, &mat, {});
    EXPECT_FLOAT_EQ(water.waves[0].amplitude, 0.2f);

    water.enableGerstnerWaves = false;
    scene::ResolveWaterWaves(water, &mat, {});
    EXPECT_FLOAT_EQ(water.waves[0].amplitude, 0.0f);
}

// --- 環境風 ------------------------------------------------------------------

TEST(WaterWaves, TailwindGrowsWavesAndHeadwindShrinksThem)
{
    asset::MaterialAsset mat = SingleWaveMaterial({ 1.0f, 0.0f }, 0.4f, 10.0f);
    mat.params[kWindResponse] = { 1.0f };
    scene::WaterComponent water;

    scene::ResolveWaterWaves(water, &mat, { true, { 1.0f, 0.0f, 0.0f }, 10.0f });
    EXPECT_GT(water.waves[0].amplitude, 0.4f);

    scene::ResolveWaterWaves(water, &mat, { true, { -1.0f, 0.0f, 0.0f }, 10.0f });
    EXPECT_LT(water.waves[0].amplitude, 0.4f);
    // 向かい風でも消し切らない。消えると風上を向いた水面だけが鏡のように止まる。
    EXPECT_GT(water.waves[0].amplitude, 0.0f);
}

TEST(WaterWaves, WindIsIgnoredWhenTheMaterialDoesNotRespond)
{
    const asset::MaterialAsset mat = SingleWaveMaterial({ 1.0f, 0.0f }, 0.4f, 10.0f);
    scene::WaterComponent water;
    scene::ResolveWaterWaves(water, &mat, { true, { 1.0f, 0.0f, 0.0f }, 10.0f });
    EXPECT_FLOAT_EQ(water.waves[0].amplitude, 0.4f);
}

TEST(WaterWaves, WindDoesNotRotateWaveDirections)
{
    // 向きを回すと、原点から遠い点ほど位相が跳んで遠景の波が走る (WaterSystem.hpp の WHY)。
    asset::MaterialAsset mat = SingleWaveMaterial({ 1.0f, 0.0f }, 0.4f, 10.0f);
    mat.params[kWindResponse] = { 1.0f };
    scene::WaterComponent water;
    scene::ResolveWaterWaves(water, &mat, { true, { 0.0f, 0.0f, 1.0f }, 10.0f });
    EXPECT_FLOAT_EQ(water.waves[0].direction.x, 1.0f);
    EXPECT_FLOAT_EQ(water.waves[0].direction.y, 0.0f);
}

// --- 水流 --------------------------------------------------------------------

TEST(WaterWaves, CurrentFollowsFlowDirectionAtCurrentSpeed)
{
    asset::MaterialAsset mat;
    mat.params[kFlowDirection] = { 0.0f, 2.0f };
    mat.params[kCurrentSpeed]  = { 1.5f };
    scene::WaterComponent water;
    scene::ResolveWaterWaves(water, &mat, {});
    EXPECT_NEAR(water.current.x, 0.0f, 1.0e-6f);
    EXPECT_NEAR(water.current.y, 1.5f, 1.0e-5f);
}

TEST(WaterWaves, NoCurrentWithoutCurrentSpeed)
{
    // flowDirection は見た目 (さざ波の流れ) にも使う。速さを入れない限り物体は押さない。
    asset::MaterialAsset mat;
    mat.params[kFlowDirection] = { 1.0f, 0.0f };
    scene::WaterComponent water;
    scene::ResolveWaterWaves(water, &mat, {});
    EXPECT_FLOAT_EQ(water.current.Length(), 0.0f);
}

// --- CPU の水面高さ ----------------------------------------------------------

TEST(WaterWaves, SurfaceHeightIsEvaluatedInWorldCoordinates)
{
    // シェーダーは位相をワールド XZ で取る。CPU も同じ座標を受けて同じ高さを返すこと。
    const asset::MaterialAsset mat = SingleWaveMaterial({ 1.0f, 0.0f }, 0.5f, 8.0f);
    scene::WaterComponent water;
    scene::ResolveWaterWaves(water, &mat, {});

    const float k     = math::TWO_PI / 8.0f;
    const float omega = std::sqrt(9.8f * k);
    const float x     = 123.0f;
    const float time  = 1.25f;
    EXPECT_NEAR(water.GetSurfaceHeightAt(x, 0.0f, time), 0.5f * std::sin(k * x - omega * time), 1.0e-4f);
}

} // namespace fbzz::tests
