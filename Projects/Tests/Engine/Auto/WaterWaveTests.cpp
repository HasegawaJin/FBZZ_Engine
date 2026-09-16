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
#include <algorithm>
#include <cmath>

namespace fbzz::tests {
namespace {

using namespace scene::water_keys;

/// 波 0 だけを持つ .mat。残りの波は振幅 0 で明示的に消す。
asset::MaterialAsset SingleWaveMaterial(math::Vector2 direction, float amplitude, float wavelength,
                                        float steepness = 0.5f)
{
    asset::MaterialAsset mat;
    for (int i = 0; i < 4; ++i)
        mat.params[kWaveAmplitude[i]] = { 0.0f };
    mat.params[kWaveDirection[0]]  = { direction.x, direction.y };
    mat.params[kWaveAmplitude[0]]  = { amplitude };
    mat.params[kWaveWavelength[0]] = { wavelength };
    mat.params[kWaveSteepness[0]]  = { steepness };
    // 位相と逆写像だけを見たいので «群» と «方向広がり» は切る。既定では 0.45 / 0.35 が入る。
    mat.params[kWaveGrouping]      = { 0.0f };
    mat.params[kWaveSpread]        = { 0.0f };
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
    // 急峻度 0 の波は水平に動かないので、ワールド XZ がそのまま位相になる。
    const asset::MaterialAsset mat = SingleWaveMaterial({ 1.0f, 0.0f }, 0.5f, 8.0f, 0.0f);
    scene::WaterComponent water;
    scene::ResolveWaterWaves(water, &mat, {});

    const float k     = math::TWO_PI / 8.0f;
    const float omega = std::sqrt(9.8f * k);
    const float x     = 123.0f;
    const float time  = 1.25f;
    EXPECT_NEAR(water.GetSurfaceHeightAt(x, 0.0f, time), 0.5f * std::sin(k * x - omega * time), 1.0e-4f);
}

TEST(WaterWaves, SurfaceHeightSolvesBackThroughTheHorizontalDisplacement)
{
    // Gerstner 波は «変位前の位置» を位相に取る。変位前の点 x0 の水面が実際に出るのは
    // x0 + Q·A·cos(phi) で、そこへ問い合わせたら A·sin(phi) が返らなければならない。
    // 位相をワールド XZ へ直接入れていた頃は、ここが Q·A ぶんずれていた。
    const asset::MaterialAsset mat = SingleWaveMaterial({ 1.0f, 0.0f }, 0.5f, 8.0f, 0.5f);
    scene::WaterComponent water;
    scene::ResolveWaterWaves(water, &mat, {});

    const float k     = math::TWO_PI / 8.0f;
    const float omega = std::sqrt(9.8f * k);
    const float time  = 1.25f;

    // 波の斜面 (cos が最大に近いところ) を選ぶ。山や谷では水平変位がゼロになり差が出ない。
    const float x0    = (0.25f * math::TWO_PI + omega * time) / k;
    const float phase = k * x0 - omega * time;
    const float displacedX = x0 + 0.5f * 0.5f * std::cos(phase);

    EXPECT_NEAR(water.GetSurfaceHeightAt(displacedX, 0.0f, time),
                0.5f * std::sin(phase), 1.0e-3f);
}

TEST(WaterWaves, SteepWavesStayBoundedWhenTheInverseIsNotUnique)
{
    // Ocean.mat のような Sum(Q·k·A) > 1 の設定では逆写像が一意でない。どの枝へ落ちても、
    // 返す高さが振幅の合計を超えない = 反復が発散していないことだけは保証する。
    asset::MaterialAsset mat;
    const float amplitudes[4]  = { 4.47f, 2.49f, 5.36f, 2.28f };
    const float wavelengths[4] = { 14.0f, 22.0f,  9.0f, 18.0f };
    const float steepness[4]   = {  0.4f,  0.3f, 0.25f,  0.2f };
    const math::Vector2 dirs[4] = {
        { 0.27f, 0.10f }, { -0.13f, 0.45f }, { 0.29f, -0.70f }, { -0.66f, 0.27f }
    };
    for (int i = 0; i < 4; ++i) {
        mat.params[kWaveDirection[i]]  = { dirs[i].x, dirs[i].y };
        mat.params[kWaveAmplitude[i]]  = { amplitudes[i] };
        mat.params[kWaveWavelength[i]] = { wavelengths[i] };
        mat.params[kWaveSteepness[i]]  = { steepness[i] };
    }
    scene::WaterComponent water;
    scene::ResolveWaterWaves(water, &mat, {});

    // 群の包絡と方向広がりが振幅を持ち上げるぶんも上界に織り込む。
    const float amplitudeSum = amplitudes[0] + amplitudes[1] + amplitudes[2] + amplitudes[3];
    const float bound = amplitudeSum * (1.0f + water.waveGrouping)
                      * scene::WaterComponent::WaveSpreadAmplitudeSum(water.waveSpread);
    for (int i = 0; i < 40; ++i) {
        const float x = -400.0f + static_cast<float>(i) * 20.0f;
        const float height = water.GetSurfaceHeightAt(x, x * 0.37f, 3.5f);
        EXPECT_LE(std::abs(height), bound);
    }
}

// --- 波の «群» --------------------------------------------------------------

TEST(WaterWaves, GroupingConstantsMatchTheAngleTheyEncode)
{
    // 定数は Water.hlsl と二重化している。数値を打ち間違えると、見えている波と浮力が
    // «少しだけ» 違う水面になり、絵でも数値でも気付けない。
    using W = scene::WaterComponent;
    EXPECT_NEAR(W::WAVE_GROUP_COS, std::cos(0.55f), 1.0e-5f);
    EXPECT_NEAR(W::WAVE_GROUP_SIN, std::sin(0.55f), 1.0e-5f);
}

TEST(WaterWaves, GroupEnvelopeIsExactlyOneWhenGroupingIsOff)
{
    // 0 にしたら «群の無い従来の波» へ完全に戻ること。既存シーンの見え方を変えない逃げ道。
    for (int i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(
            scene::WaterComponent::WaveGroupEnvelope(i, { 1.0f, 0.0f }, 0.45f, 2.1f,
                                                     37.0f, -11.0f, 4.25f, 0.0f),
            1.0f);
    }
}

TEST(WaterWaves, GroupEnvelopeSweepsTheWholeGroupingDepth)
{
    // 包絡が [1-d, 1+d] に収まり、かつ両端まで実際に振れること。振れなければ «群» にならない。
    constexpr float depth = 0.45f;
    float lowest = 2.0f;
    float highest = 0.0f;
    for (int i = 0; i < 400; ++i) {
        const float x = static_cast<float>(i) * 1.7f;
        const float value = scene::WaterComponent::WaveGroupEnvelope(
            0, { 1.0f, 0.0f }, 0.45f, 2.1f, x, 0.0f, 0.0f, depth);
        EXPECT_GE(value, 1.0f - depth - 1.0e-4f);
        EXPECT_LE(value, 1.0f + depth + 1.0e-4f);
        lowest = (std::min)(lowest, value);
        highest = (std::max)(highest, value);
    }
    EXPECT_LT(lowest, 1.0f - depth * 0.95f);
    EXPECT_GT(highest, 1.0f + depth * 0.95f);
}

TEST(WaterWaves, GroupsTravelAtHalfThePhaseSpeed)
{
    // 深水波の群速度は位相速度の 1/2。«群はゆっくり進み、波頭がその中を追い越していく»
    // という海面の見え方はここで決まる。
    constexpr float k = 0.45f;
    constexpr float omega = 2.1f;
    constexpr float depth = 0.45f;
    const math::Vector2 direction = { 1.0f, 0.0f };
    const float groupSpeed = omega / (2.0f * k);

    // 群が進む向き。波から WAVE_GROUP_COS / SIN ぶん傾いている。
    using W = scene::WaterComponent;
    const math::Vector2 groupDir = { W::WAVE_GROUP_COS, W::WAVE_GROUP_SIN };

    for (int i = 0; i < 8; ++i) {
        const float t = static_cast<float>(i) * 0.9f;
        const float travel = groupSpeed * t;
        const float atOrigin = W::WaveGroupEnvelope(0, direction, k, omega, 12.0f, -5.0f, 0.0f, depth);
        const float carried  = W::WaveGroupEnvelope(0, direction, k, omega,
                                                    12.0f + groupDir.x * travel,
                                                    -5.0f + groupDir.y * travel, t, depth);
        EXPECT_NEAR(atOrigin, carried, 1.0e-3f);
    }
}

// --- 方向広がり --------------------------------------------------------------

TEST(WaterWaves, WaveSpreadKeepsTheTotalEnergy)
{
    // 主成分と伴走成分は振幅を «分け合う»。二乗和が 1 でないと、spread を上げるだけで
    // 海全体が高く (or 低く) なり、他のパラメータを詰め直す羽目になる。
    using W = scene::WaterComponent;
    for (float spread : { 0.0f, 0.2f, 0.35f, 0.7f, 1.0f }) {
        const auto main = W::WaveSpreadComponent(0, 0, { 1.0f, 0.0f }, spread);
        const auto comp = W::WaveSpreadComponent(0, 1, { 1.0f, 0.0f }, spread);
        EXPECT_NEAR(main.amplitudeScale * main.amplitudeScale
                    + comp.amplitudeScale * comp.amplitudeScale, 1.0f, 1.0e-5f);
    }
}

TEST(WaterWaves, WaveSpreadOfZeroLeavesASingleWave)
{
    // 0 にしたら «1 波 1 方向» の従来へ完全に戻ること。既存シーンの逃げ道。
    using W = scene::WaterComponent;
    const auto main = W::WaveSpreadComponent(1, 0, { 0.6f, 0.8f }, 0.0f);
    const auto comp = W::WaveSpreadComponent(1, 1, { 0.6f, 0.8f }, 0.0f);
    EXPECT_FLOAT_EQ(main.amplitudeScale, 1.0f);
    EXPECT_FLOAT_EQ(comp.amplitudeScale, 0.0f);
    EXPECT_FLOAT_EQ(main.direction.x, 0.6f);
    EXPECT_FLOAT_EQ(main.direction.y, 0.8f);
}

TEST(WaterWaves, WaveSpreadRotatesTheCompanionAndAlternatesItsSide)
{
    // 伴走成分は «同じ波数で向きだけ違う» 波。向きを回さなければ短い波頭にならない。
    // 全部同じ側へ回すと海全体が傾くので、波の番号で左右を入れ替える。
    using W = scene::WaterComponent;
    constexpr float spread = 0.5f;
    const math::Vector2 direction = { 1.0f, 0.0f };

    const auto even = W::WaveSpreadComponent(0, 1, direction, spread);
    const auto odd  = W::WaveSpreadComponent(1, 1, direction, spread);

    // 長さは変わらない (向きを回すだけ)。
    EXPECT_NEAR(even.direction.Length(), 1.0f, 1.0e-5f);
    EXPECT_NEAR(std::atan2(even.direction.y, even.direction.x), spread * 0.9f, 1.0e-5f);
    EXPECT_NEAR(std::atan2(odd.direction.y, odd.direction.x), -spread * 0.9f, 1.0e-5f);
}

} // namespace fbzz::tests
