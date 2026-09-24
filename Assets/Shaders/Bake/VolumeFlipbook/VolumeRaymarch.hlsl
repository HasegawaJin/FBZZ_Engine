/// @file    VolumeRaymarch.hlsl
/// @brief   Volume Flipbook Baker: ボリュームを平行投影でレイマーチし、色と画面空間速度を横並びに書く。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// @note RT は 6·tile × tile に 6 枚のタイルを横に並べる:
/// @note 0 は事前乗算のリニア HDR 色 (a = 1 - 透過率)。
/// @note gDistortion なら覆い付き平均速度 (右, 上) と 1 - 透過率を格納し、タイル 1〜5 は描かない。
/// @note 1 は画面空間速度 (タイル UV/秒) と重みの合計を格納する。
/// @note 2 は 6 方向ライトマップ Positive (右, 上, 奥, α) で、gSixWay が 0 なら描かない。
/// @note 3 は 6 方向ライトマップ Negative (左, 下, 手前, 発光マスク)。
/// @note 6 方向マップの規約は Engine/Asset/SixWayLighting.hpp を参照する (単位白色光の明るさ、モノクロ・リニア・ストレート)。
/// @note 4 は覆い率で重み付けした albedo ramp 色相、5 は積分済み emission RGB (Fire はソフトニー済み)。
/// @note CPU へ読み戻せる色は color 0 だけなので、各 tile の値を 1 枚へ横並びにする。
/// @note MRT にするとバックエンドの改修が要り、RT を 2 枚にすると GPU 待ちが 2 回になる。
///
/// @note 媒質は煙と液体の 2 種類が混ざる (A = 液体の割合)。
/// @note 煙は散乱する霧、液体は gLiquidThreshold を跨ぐ密度面を法線の陰影で描く。
#include "Common/Fullscreen.hlsli"
#include "Common/Color.hlsli"
/// @note FbmNoise3D
#include "Rendering/ParticleNoise.hlsli"
#include "Common/BindlessIndices.hlsli"

cbuffer VolumeRaymarchConstants : register(b0)
{
    float3 gCamRight;   float gHalfExtent;
    float3 gCamUp;      uint  gTileSize;
    float3 gCamForward; uint  gRaySteps;
    float3 gToLight;    uint  gShadowSteps;
    float3 gLightColor; float gExtinction;
    float3 gAmbient;    float gEmissionIntensity;
    /// @note 0 = ベイク用の生値 / 1 = プレビュー (色) / 2 = プレビュー (α)
    uint  gDisplayMode;
    float gAnisotropy;
    float gPreviewMotionScale;
    /// @note プレビューの背景。0 = 暗 / 1 = 明 / 2 = チェッカー
    uint  gBackground;
    float gExposure;
    float gLiquidThreshold;
    float gLiquidSoftness;
    float gLiquidExtinction;
    float gLiquidSpecular;
    float gLiquidGloss;
    float gLiquidF0;
    /// @note 1 ボクセルの幅 [bake 単位]
    float gVoxelSize;
    /// @note rgb = 色 / w = 位置 (昇順)
    float4 gAlbedoRamp[4];
    float4 gEmissionRamp[4];
    /// @note 1 = タイル 2 / 3 に 6 方向ライトマップを描く
    uint  gSixWay;
    /// @note 多重散乱の段数 (1 = 単散乱)
    uint  gOctaves;
    /// @note 1 = 発光の色を黒体放射で決める
    uint  gBlackbody;
    /// @note 標本位置のずれをコマごとに変える
    uint  gFrameIndex;
    /// @note 環境光を上の煙が遮る割合
    float gSkyOcclusion;
    /// @note 格子より細かい起伏の強さ (0 で無効)
    float gDetailStrength;
    float gDetailScale;
    /// @note 起伏を流れに乗せて入れ替える周期 [秒]
    float gDetailPeriod;
    /// @note このコマの時刻 [秒]
    float gTime;
    /// @note 黒体放射の色の下限 (これより冷たい所も色はこの温度)
    float gKelvinMin;
    /// @note 温度 1 に当たる色温度
    float gKelvinMax;
    /// @note 1 = 色の代わりに歪みマップの生値を描く
    uint  gDistortion;
    /// @note プレビューの符号化にだけ使う (焼きの符号化は CPU)
    float gDistortionScale;
    /// @note 1 = Fire の放射を密度と独立して積分する。
    uint  gFireEmission;
    /// @note Fire の 6-way マスクと MV 重みに使う係数。放射 q(T) には掛けない。
    float gFireEmissionExtinction;
    float gDistortionPad2;
    float gBlackbodyLutMaxKelvin;
    float3 gBlackbodyLutPad;
    float4 gBlackbodyColorLut[256];
};

/// @note R 密度 / G 温度 / B colorKey / A 液体の割合
FBZZ_TEX3D_T(float4, gMedium, 0);
FBZZ_TEX3D_T(float4, gVelocity, 1);
SamplerState      gLinearClamp : register(s2);

/// @note 背景は «表示の色» (sRGB) で決め、合成のためにリニアへ戻す。
float3 PreviewBackground(float2 local)
{
    if (gBackground == 1u) return SRGBToLinear(float3(0.82f, 0.82f, 0.84f));
    if (gBackground == 2u)
    {
        const uint2 cell = uint2(local / 8.0f);
        const float3 light = float3(56.0f / 255.0f, 56.0f / 255.0f, 56.0f / 255.0f);
        const float3 dark = float3(31.0f / 255.0f, 31.0f / 255.0f, 31.0f / 255.0f);
        return SRGBToLinear(((cell.x + cell.y) & 1u) != 0u ? light : dark);
    }
    return SRGBToLinear(float3(8.0f / 255.0f, 8.0f / 255.0f, 10.0f / 255.0f));
}

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

bool IntersectUnitBox(float3 origin, float3 direction, out float tNear, out float tFar)
{
    /// @note 軸に平行な成分を 0 のまま割ると、原点が面上にある画素で 0·inf = NaN が出る。
    float3 safeDirection = direction;
    safeDirection.x = abs(safeDirection.x) < 1.0e-6f ? 1.0e-6f : safeDirection.x;
    safeDirection.y = abs(safeDirection.y) < 1.0e-6f ? 1.0e-6f : safeDirection.y;
    safeDirection.z = abs(safeDirection.z) < 1.0e-6f ? 1.0e-6f : safeDirection.z;
    const float3 inverse = 1.0f / safeDirection;
    const float3 t0 = (-1.0f - origin) * inverse;
    const float3 t1 = ( 1.0f - origin) * inverse;
    const float3 tMin = min(t0, t1);
    const float3 tMax = max(t0, t1);
    tNear = max(max(tMin.x, tMin.y), tMin.z);
    tFar  = min(min(tMax.x, tMax.y), tMax.z);
    return tFar > max(tNear, 0.0f);
}

float4 MediumAt(float3 position)
{
    return gMedium.SampleLevel(gLinearClamp, position * 0.5f + 0.5f, 0.0f);
}

/// @note 引く位置そのものをノイズで数ボクセルずらす (domain warp)。
///
/// @note 値だけを揺らす DetailFactor では格子の輪郭が残るため、座標も歪めて輪郭を波打たせる。
/// @note 体積解像度や焼き時間を増やさず、見た目の細かさを上げる。
/// @note 影側は位置歪みを省き、1 オクターブの濃淡だけを追加する。
/// @note 影の行進は視線標本ごとに shadow_steps 回走るため、追加ノイズ 3 回は高コストになる。
/// @note 定数バッファのレイアウトを維持するため、歪み量は detailStrength から導く。
/// @note レイアウト変更は C++ の static_assert と Script DLL の再ビルドへ波及する。
float3 WarpForDetail(float3 position)
{
    if (gDetailStrength <= 0.0f) return position;
    const float3 q = position * gDetailScale;
    const float3 offset = float3(ValueNoise3D(q), ValueNoise3D(q + 31.7f), ValueNoise3D(q + 71.3f));
    /// @note 2 ボクセルぶんを上限にする。これ以上ずらすと «別の場所の煙» を引いて形が崩れる。
    return position + offset * (gDetailStrength * 2.0f * gVoxelSize);
}

/// @note 画素ごとの標本位置のずれ [0,1)。固定の 0.5 だと、行進の刻みの境目が等高線の縞として残る。
static float gPixelJitter = 0.5f;

/// @note Interleaved Gradient Noise (Jimenez 2014)。隣の画素と相関が低く、縮めたときに縞が消える。
float InterleavedGradientNoise(float2 pixel)
{
    return frac(52.9829189f * frac(dot(pixel, float2(0.06711056f, 0.00583715f))));
}

/// @note 格子より細かい起伏。ノイズの座標を «その場の速度» で 2 層ずらし、半周期ずらして混ぜる
/// @note (Neyret 2003, Advected Textures)。流れと一緒に動くので、止まったノイズが煙の上を滑らない。
float DetailFactor(float3 position)
{
    if (gDetailStrength <= 0.0f) return 1.0f;
    const float3 v = gVelocity.SampleLevel(gLinearClamp, position * 0.5f + 0.5f, 0.0f).xyz;
    const float period = max(gDetailPeriod, 0.05f);
    const float phase0 = frac(gTime / period);
    const float phase1 = frac(gTime / period + 0.5f);
    const float weight0 = 1.0f - abs(2.0f * phase0 - 1.0f);
    const float weight1 = 1.0f - weight0;
    /// @note FluidRenderMath.hpp と同じ 4 オクターブ、振幅和、層オフセットを使う。
    const float n0 = FbmNoise3D((position - v * (phase0 * period)) * gDetailScale, 4) / 0.9375f;
    const float n1 = FbmNoise3D((position - v * (phase1 * period)) * gDetailScale + 17.31f, 4) / 0.9375f;
    /// @note 2 層を重みで混ぜるとノイズの振幅が縮む (中間で最大 1/√2)。戻さないと detail_period の半分の
    /// @note 周期でディテールのコントラストが脈打つ (FluidRenderMath.hpp の FluidDetailFactor と同じ補正)。
    const float blended = (weight0 * n0 + weight1 * n1)
                        * rsqrt(max(weight0 * weight0 + weight1 * weight1, 1.0e-4f));
    /// @note 上を saturate で切ると正の山だけが 1.0 で潰れ、密度を削る方向にしか効かなくなる。下だけ止める。
    ///
    return max(1.0f + gDetailStrength * blended, 0.0f);
}

/// @note 視線の行進で使う媒質。ここは «輪郭の歪み + 流れに乗せた濃淡» の両方を足す (影側は濃淡だけ)。
/// @note 液体には掛けない: 密度の等値面が液面なので、ノイズで削ると液面が虫食いになる。
float4 ViewMediumAt(float3 position)
{
    /// @note 液体 (medium.a >= 0.5) は等値面で表面を出すので歪めない。歪めると液面が泡立って見える。
    const float4 straight = MediumAt(position);
    if (straight.a >= 0.5f) return straight;
    /// @note 2D Distortion と同じく、速度符号化の覆いには描画用の細部を入れない。
    if (gDistortion != 0u) return straight;
    float4 medium = MediumAt(WarpForDetail(position));
    medium.a = straight.a;
    if (medium.r > 1.0e-5f || medium.g > 1.0e-5f)
    {
        /// @note Fire / Glow の可視光は温度から作る。密度だけを動かすと炎は静止して見える。
        /// @note 2D の FluidBaker と同じ倍率を両方へ掛け、速度に沿う上昇を発光にも渡す。
        const float detail = DetailFactor(position);
        medium.r *= detail;
        medium.g *= detail;
    }
    return medium;
}

/// @note VolumeFlipbookBaker.cpp の EvaluateVolumeRamp と同じ。位置は CPU 側で昇順に揃えてある。
float3 EvaluateRamp(float4 s0, float4 s1, float4 s2, float4 s3, float t)
{
    t = saturate(t);
    if (t <= s0.w) return s0.rgb;
    if (t <= s1.w) return lerp(s0.rgb, s1.rgb, saturate((t - s0.w) / max(s1.w - s0.w, 1.0e-5f)));
    if (t <= s2.w) return lerp(s1.rgb, s2.rgb, saturate((t - s1.w) / max(s2.w - s1.w, 1.0e-5f)));
    if (t <= s3.w) return lerp(s2.rgb, s3.rgb, saturate((t - s2.w) / max(s3.w - s2.w, 1.0e-5f)));
    return s3.rgb;
}

float LiquidCoverage(float density)
{
    return smoothstep(gLiquidThreshold - gLiquidSoftness, gLiquidThreshold + gLiquidSoftness, density);
}

/// @note x = 煙の消散係数 / y = 液体の消散係数。視線の行進と影の行進で同じ式を使う。
float2 Extinctions(float4 medium)
{
    const float liquid = saturate(medium.a);
    return float2(medium.r * (1.0f - liquid) * gExtinction,
                  LiquidCoverage(medium.r) * liquid * gLiquidExtinction);
}

/// @note toLight の向きへの光学的厚さ (∫σ ds)。影を落とすのは同じボリュームだけ。
float OpticalDepthAlong(float3 position, float3 toLight)
{
    float tNear, tFar;
    if (!IntersectUnitBox(position, toLight, tNear, tFar))
        return 0.0f;
    const float stepLength = tFar / float(max(gShadowSteps, 1u));
    float opticalDepth = 0.0f;
    [loop] for (uint i = 0; i < gShadowSteps; ++i)
    {
        /// @note 細部を «遮る側» にも入れる。素の格子だけで測ると、視線側で足した起伏に陰影が付かず
        /// @note 平面的な模様に見える。
        ///
        /// @note 影側で 1 オクターブの濃淡を使う理由: この行は視線 1 標本につき
        /// @note shadow_steps 回走る。視線側と同じ厚さ (ノイズ 11 回) にすると焼き時間が桁で跳ねる。
        /// @note 遮光に効くのは «濃いか薄いか» なので、1 回のノイズで足りる。
        const float3 at = position + toLight * (stepLength * (float(i) + gPixelJitter));
        float4 medium = MediumAt(at);
        if (gDetailStrength > 0.0f && medium.a < 0.5f)
            medium.r *= max(1.0f + gDetailStrength * ValueNoise3D(at * gDetailScale), 0.0f);
        const float2 sigma = Extinctions(medium);
        opticalDepth += sigma.x + sigma.y;
    }
    return opticalDepth * stepLength;
}

float LightTransmittanceAlong(float3 position, float3 toLight)
{
    return exp(-OpticalDepthAlong(position, toLight));
}

float LightTransmittance(float3 position)
{
    return LightTransmittanceAlong(position, gToLight);
}

/// @note 4π を掛けて等方散乱が 1 になるよう正規化した Henyey-Greenstein。
float PhaseHG(float cosTheta, float g)
{
    const float denominator = 1.0f + g * g - 2.0f * g * cosTheta;
    return (1.0f - g * g) / pow(max(denominator, 1.0e-4f), 1.5f);
}

/// @note 多重散乱の近似 (Wrenninge et al. 2013, "Oz: The Great and Volumetric")。
/// @note 消散を a^i・寄与を b^i・位相の偏りを c^i で弱めた «光の段» を重ねる (a = b = c = 0.5)。
/// @note 単散乱だけだと煙の奥が真っ黒に落ち、実物の «内側から明るい» 柔らかさが出ない。
float MultiScatter(float opticalDepth, float cosTheta)
{
    float sum = 0.0f;
    float a = 1.0f;
    float b = 1.0f;
    float c = 1.0f;
    [loop] for (uint i = 0; i < max(gOctaves, 1u); ++i)
    {
        sum += b * PhaseHG(cosTheta, gAnisotropy * c) * exp(-opticalDepth * a);
        a *= 0.5f;
        b *= 0.5f;
        c *= 0.5f;
    }
    return sum;
}

/// @note 6 方向マップと天空光の遮蔽用。位相を掛けず、段の重みの和で割って [0,1] に保つ
/// @note (マップは «単位の光への応答» なので 1 を超えると 8bit で飽和する)。
float MultiScatterIsotropic(float opticalDepth)
{
    float sum = 0.0f;
    float weightSum = 0.0f;
    float a = 1.0f;
    float b = 1.0f;
    [loop] for (uint i = 0; i < max(gOctaves, 1u); ++i)
    {
        sum += b * exp(-opticalDepth * a);
        weightSum += b;
        a *= 0.5f;
        b *= 0.5f;
    }
    return sum / weightSum;
}

/// @note 環境光のうち «上に積もった煙» を抜けて届く割合。煙の下側が暗くなり、塊に重さが出る。
float SkyVisibility(float3 position)
{
    if (gSkyOcclusion <= 0.0f) return 1.0f;
    return lerp(1.0f, MultiScatterIsotropic(OpticalDepthAlong(position, float3(0.0f, 1.0f, 0.0f))), gSkyOcclusion);
}

/// @note 非 Fluid の Volume ソース用色温度近似。Fluid Fire は 2D の ParticleBlackbodyChroma LUT を使う。
float3 ApproximateBlackbodyColor(float kelvin)
{
    const float t = clamp(kelvin, 1000.0f, 15000.0f);
    const float t2 = t * t;
    const float u = (0.860117757f + 1.54118254e-4f * t + 1.28641212e-7f * t2)
                  / (1.0f + 8.42420235e-4f * t + 7.08145163e-7f * t2);
    const float v = (0.317398726f + 4.22806245e-5f * t + 4.20481691e-8f * t2)
                  / (1.0f - 2.89741816e-5f * t + 1.61456053e-7f * t2);
    const float denom = 2.0f * u - 8.0f * v + 4.0f;
    const float x = 3.0f * u / denom;
    const float y = 2.0f * v / denom;
    const float3 xyz = float3(x / y, 1.0f, (1.0f - x - y) / y);
    float3 rgb = float3(dot(float3(3.2404542f, -1.5371385f, -0.4985314f), xyz),
                        dot(float3(-0.9692660f, 1.8760108f, 0.0415560f), xyz),
                        dot(float3(0.0556434f, -0.2040259f, 1.0572252f), xyz));
    rgb = max(rgb, 0.0f);
    return rgb / max(max(rgb.r, rgb.g), max(rgb.b, 1.0e-6f));
}

/// @note ParticleBlackbodyChroma LUT を 2D FluidBaker と同じ Kelvin 範囲・線形補間で読む。
float3 FluidFireBlackbodyColor(float kelvin)
{
    const float position = saturate(kelvin / max(gBlackbodyLutMaxKelvin, 1.0f)) * 255.0f;
    const uint index = min((uint)position, 254u);
    return lerp(gBlackbodyColorLut[index].rgb, gBlackbodyColorLut[index + 1u].rgb,
                position - float(index));
}

/// @note Fire の温度は最大発生源で正規化されるが 1 を超えることがあるため、T⁴ と色温度の比を保つ。
/// @note 黒体放射の輝度は温度の 4 乗 (Stefan-Boltzmann) で、芯だけが白く光る。
float3 EmissionAt(float temperature)
{
    const float t = gFireEmission != 0u ? max(temperature, 0.0f) : saturate(temperature);
    if (gBlackbody != 0u)
    {
        const float3 chroma = gFireEmission != 0u
            ? FluidFireBlackbodyColor(gKelvinMax * t)
            : ApproximateBlackbodyColor(max(gKelvinMax * t, gKelvinMin));
        return gEmissionIntensity * (t * t * t * t) * chroma;
    }
    if (gFireEmission != 0u)
        /// @note 2D Fire は Ramp を温度から引き、fireIntensity を掛けるため、ここでは追加の T² を掛けない。
        return gEmissionIntensity
            * EvaluateRamp(gEmissionRamp[0], gEmissionRamp[1], gEmissionRamp[2], gEmissionRamp[3], t);
    return gEmissionIntensity * t * t
        * EvaluateRamp(gEmissionRamp[0], gEmissionRamp[1], gEmissionRamp[2], gEmissionRamp[3], t);
}

/// @brief FluidFireRendering.hpp と同じ一定消散区間の平均透過率。
float FluidFireMeanTransmittance(float transmittance, float extinction, float segmentLength)
{
    const float opticalDepth = max(extinction, 0.0f) * max(segmentLength, 0.0f);
    const float attenuation = opticalDepth > 1.0e-5f
        ? (1.0f - exp(-opticalDepth)) / opticalDepth
        : 1.0f - 0.5f * opticalDepth;
    return saturate(transmittance) * attenuation;
}

/// @brief 2D Fire と積分済みの 3D Fire を同じ 8bit 範囲へ収める。
float3 FluidFireSoftKnee(float3 radiance)
{
    return 1.0f - exp(-max(radiance, 0.0f));
}

/// @note 6 方向マップの発光マスク。EmissionAt の輝度の形だけを取り出したもの。
float EmissionMask(float temperature)
{
    const float t = gFireEmission != 0u ? max(temperature, 0.0f) : saturate(temperature);
    return gBlackbody != 0u ? t * t * t * t : t * t;
}

float3 LiquidNormal(float3 position)
{
    const float h = gVoxelSize;
    const float3 gradient = float3(
        MediumAt(position + float3(h, 0.0f, 0.0f)).r - MediumAt(position - float3(h, 0.0f, 0.0f)).r,
        MediumAt(position + float3(0.0f, h, 0.0f)).r - MediumAt(position - float3(0.0f, h, 0.0f)).r,
        MediumAt(position + float3(0.0f, 0.0f, h)).r - MediumAt(position - float3(0.0f, 0.0f, h)).r);
    /// @note 密度が減る向きが外側。
    return dot(gradient, gradient) > 1.0e-10f ? -normalize(gradient) : -gCamForward;
}

float3 ShadeLiquid(float3 position, float3 albedo)
{
    const float3 n = LiquidNormal(position);
    const float3 v = -gCamForward;
    const float3 h = normalize(gToLight + v);
    /// @note 表面から少し浮かせて影を測る。表面より内側の標本から測ると、自分の中身で真っ暗になる。
    const float shadow = LightTransmittance(position + n * (gVoxelSize * 1.5f));
    /// @note 回り込み (wrap)。液滴は小さく、裏から光が抜けるので影側を真っ黒にしない。
    const float diffuse = saturate((dot(n, gToLight) + 0.35f) / 1.35f);
    const float fresnel = gLiquidF0 + (1.0f - gLiquidF0) * pow(1.0f - saturate(dot(n, v)), 5.0f);
    const float highlight = gLiquidSpecular * pow(saturate(dot(n, h)), max(gLiquidGloss, 1.0f));
    const float3 body = albedo * (gLightColor * diffuse * shadow + gAmbient) * (1.0f - fresnel);
    /// @note ベイク空間に空は無いので、映り込みは環境光の色で近似する。
    const float3 reflection = fresnel * gAmbient * 2.0f + gLightColor * highlight * shadow;
    return body + reflection;
}

float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    const uint2 pixel = uint2(p.svPosition.xy);
    const uint tile = min(pixel.x / max(gTileSize, 1u), 5u);
    const bool motionHalf = tile == 1u;
    const bool sixWayTile = tile == 2u || tile == 3u;
    const bool sixWayAlbedoColorTile = tile == 4u;
    const bool sixWayEmissionColorTile = tile == 5u;
    const float2 local = float2(pixel.x - tile * gTileSize, pixel.y) + 0.5f;
    if ((tile >= 2u && gSixWay == 0u) || (tile != 0u && gDistortion != 0u))
        return gDisplayMode != 0u ? float4(pow(PreviewBackground(local), 1.0f / 2.2f), 1.0f) : 0.0f;
    gPixelJitter = InterleavedGradientNoise(float2(pixel) + float(gFrameIndex) * 5.588238f);
    const float2 screen = float2(local.x / float(gTileSize) * 2.0f - 1.0f,
                                 1.0f - local.y / float(gTileSize) * 2.0f);

    const float3 origin = (gCamRight * screen.x + gCamUp * screen.y) * gHalfExtent - gCamForward * 4.0f;
    float tNear, tFar;
    float3 color = 0.0f;
    float3 fireRadiance = 0.0f;
    float transmittance = 1.0f;
    float fireTransmittance = 1.0f;
    float weightSum = 0.0f;
    float2 motionSum = 0.0f;
    float2 distortionSum = 0.0f;
    /// @note 6 方向: このタイルが持つ 3 軸 (Positive = +右/+上/+奥、Negative = その逆) の明るさと発光。
    float3 lightSum = 0.0f;
    float3 albedoColorSum = 0.0f;
    float3 emissionRadiance = 0.0f;
    float emissionSum = 0.0f;
    const float axisSign = tile == 2u ? 1.0f : -1.0f;

    if (IntersectUnitBox(origin, gCamForward, tNear, tFar))
    {
        tNear = max(tNear, 0.0f);
        const float stepLength = (tFar - tNear) / float(max(gRaySteps, 1u));
        const float cosLight = dot(gToLight, gCamForward);
        [loop] for (uint i = 0; i < gRaySteps; ++i)
        {
            const float3 position = origin + gCamForward * (tNear + stepLength * (float(i) + gPixelJitter));
            const float4 medium = ViewMediumAt(position);
            const float temperature = gFireEmission != 0u ? max(medium.g, 0.0f) : saturate(medium.g);
            const float emissionMask = EmissionMask(temperature);
            float fireWeight = 0.0f;
            if (gFireEmission != 0u && emissionMask > 1.0e-6f)
            {
                /// @see https://developer.nvidia.com/gpugems/gpugems3/part-v-physics-simulation/chapter-30-real-time-simulation-and-rendering-3d-fluids GPU Gems 3, §30.3.1 Fire.
                /// @note Fire マスクと MV の重み。熱放射の密度積分には使わない。
                const float fireSigma = gFireEmissionExtinction * emissionMask;
                const float fireSampleTransmittance = exp(-fireSigma * stepLength);
                fireWeight = transmittance * fireTransmittance * (1.0f - fireSampleTransmittance);
                fireTransmittance *= fireSampleTransmittance;
            }
            const float2 sigma = Extinctions(medium);
            const float sigmaSum = sigma.x + sigma.y;
            const bool hasFireEmission = gFireEmission != 0u && emissionMask > 1.0e-6f;
            if (sigmaSum <= 1.0e-6f && fireWeight <= 1.0e-6f && !hasFireEmission)
                continue;
            const float sampleTransmittance = exp(-sigmaSum * stepLength);
            if (hasFireEmission)
            {
                /// @note 区間内の消散が一定とみなし、煙を通る平均透過率で放射密度を積分する。
                const float meanTransmittance = FluidFireMeanTransmittance(transmittance, sigmaSum, stepLength);
                /// @see https://developer.nvidia.com/gpugems/gpugems3/part-v-physics-simulation/chapter-30-real-time-simulation-and-rendering-3d-fluids GPU Gems 3, §30.3.1 Fire.
                /// @note q(T) は煙密度と独立した黒体放射密度。全長 2 の一様場を 2D の q(T) と一致させる。
                fireRadiance += EmissionAt(temperature)
                    * (meanTransmittance * stepLength / 2.0f);
            }
            /// @note この標本が画素へ寄与する割合。速度の重みも同じ量を使う (見えている媒質の動きを取る)。
            const float weight = transmittance * (1.0f - sampleTransmittance);

            if (motionHalf)
            {
                const float3 v = gVelocity.SampleLevel(gLinearClamp, position * 0.5f + 0.5f, 0.0f).xyz;
                /// @note 画像は +V が下なので up 成分の符号を反転する。タイルは 2·halfExtent を覆う。
                const float motionWeight = gFireEmission != 0u ? max(weight, fireWeight) : weight;
                motionSum += motionWeight * float2(dot(v, gCamRight), -dot(v, gCamUp)) / (2.0f * gHalfExtent);
            }
            else if (sixWayTile || sixWayAlbedoColorTile || sixWayEmissionColorTile)
            {
                const float3 albedo = EvaluateRamp(gAlbedoRamp[0], gAlbedoRamp[1], gAlbedoRamp[2], gAlbedoRamp[3],
                                                   medium.b);
                if (sixWayTile)
                {
                    /// @note 6-way の向き別マップは反射率と輸送を保持し、Scene shader が色相を補正する。
                    /// @note 位相関数はランタイムの視線方向が必要なため、ここでは含めない。
                    const float reflectance = dot(albedo, float3(0.2126f, 0.7152f, 0.0722f));
                    lightSum += weight * reflectance * float3(
                        MultiScatterIsotropic(OpticalDepthAlong(position, gCamRight * axisSign)),
                        MultiScatterIsotropic(OpticalDepthAlong(position, gCamUp * axisSign)),
                        MultiScatterIsotropic(OpticalDepthAlong(position, gCamForward * axisSign)));
                    emissionSum += gFireEmission != 0u ? fireWeight : weight * emissionMask;
                }
                else if (sixWayAlbedoColorTile)
                    albedoColorSum += weight * saturate(albedo);
                else if (gFireEmission == 0u)
                    emissionRadiance += weight * EmissionAt(temperature);
            }
            else if (gDistortion != 0u)
            {
                /// @note 見えている媒質の動きを画面の右・上へ投影して積む (MV と同じ重み)。陰影は要らない。
                const float3 v = gVelocity.SampleLevel(gLinearClamp, position * 0.5f + 0.5f, 0.0f).xyz;
                distortionSum += weight * float2(dot(v, gCamRight), dot(v, gCamUp));
            }
            else
            {
                const float3 albedo = EvaluateRamp(gAlbedoRamp[0], gAlbedoRamp[1], gAlbedoRamp[2], gAlbedoRamp[3],
                                                   medium.b);
                float3 radiance = 0.0f;
                if (sigmaSum > 1.0e-6f)
                {
                    if (sigma.x > 0.0f)
                        radiance += sigma.x * albedo * (gLightColor * MultiScatter(OpticalDepthAlong(position, gToLight), cosLight)
                                                       + gAmbient * SkyVisibility(position));
                    if (sigma.y > 0.0f)
                        radiance += sigma.y * ShadeLiquid(position, albedo);
                    radiance /= sigmaSum;
                }

                if (gFireEmission != 0u)
                {
                    color += weight * radiance;
                }
                else
                {
                    color += weight * (radiance + EmissionAt(temperature));
                }
            }
            weightSum += motionHalf && gFireEmission != 0u ? max(weight, fireWeight) : weight;
            transmittance *= sampleTransmittance;
            if (transmittance < 1.0e-3f)
                break;
        }
    }

    const float3 background = PreviewBackground(local);
    if (sixWayTile)
    {
        /// @note ストレートで持つ (粒子の不透明度は Positive の α が決める)。
        const float coverage = 1.0f - transmittance;
        const float inverseCoverage = 1.0f / max(coverage, 1.0e-4f);
        const float3 lightmap = saturate(lightSum * inverseCoverage);
        if (gDisplayMode != 0u)
            return float4(LinearToSRGB(max(lerp(background, lightmap, coverage), 0.0f)), 1.0f);
        const float negativeAlpha = gFireEmission != 0u ? saturate(emissionSum) : saturate(emissionSum * inverseCoverage);
        return float4(lightmap, tile == 2u ? coverage : negativeAlpha);
    }
    if (sixWayAlbedoColorTile)
    {
        const float coverage = 1.0f - transmittance;
        return float4(albedoColorSum / max(weightSum, 1.0e-4f), coverage);
    }
    if (sixWayEmissionColorTile)
    {
        const float3 emissionColor = gFireEmission != 0u
            ? FluidFireSoftKnee(fireRadiance) : emissionRadiance;
        return float4(emissionColor, 1.0f);
    }
    if (motionHalf)
    {
        const float2 motion = motionSum / max(weightSum, 1.0e-5f);
        if (gDisplayMode != 0u)
        {
            /// @note 赤 = 右へ / 緑 = 下へ。静止は (0.5, 0.5) の灰色。
            const float3 visual = SRGBToLinear(float3(saturate(0.5f + motion * gPreviewMotionScale), 0.5f));
            return float4(LinearToSRGB(max(lerp(background, visual, saturate(weightSum)), 0.0f)), 1.0f);
        }
        return float4(motion, weightSum, 1.0f);
    }

    if (gDistortion != 0u)
    {
        const float coverage = 1.0f - transmittance;
        /// @note 焼きには符号化前の平均速度を返す。符号化 (0.5 中心・倍率・頭打ち) は CPU の EncodeVolumeDistortion が
        /// @note 行う。ループの速度は符号化する前に混ぜる。符号化後に混ぜると、頭打ちの位置で向きがずれる。
        const float2 velocity = distortionSum / max(weightSum, 1.0e-5f);
        if (gDisplayMode == 2u)
            return float4(coverage.xxx, 1.0f);
        if (gDisplayMode == 1u)
        {
            /// @note EncodeVolumeDistortion の写し (画像は +V が下。速さ 1 で頭打ち → 倍率 → 長さ 0.5 の安全網)。
            const float speed = length(velocity);
            const float gain = speed > 1.0e-5f ? min(speed, 1.0f) / speed * gDistortionScale : 0.0f;
            float2 d = float2(velocity.x, -velocity.y) * gain;
            const float len = length(d);
            if (len > 0.5f)
                d *= 0.5f / len;
            const float3 encoded = float3(saturate(0.5f + d), 0.5f);
            return float4(LinearToSRGB(lerp(background, SRGBToLinear(encoded), coverage)), 1.0f);
        }
        return float4(velocity, 0.0f, coverage);
    }

    const float alpha = 1.0f - transmittance;
    /// @note 2D Fire と同じ 1-exp(-x) のソフトニーで熱放射を 8bit 表現へ収める。
    const float3 fireColor = gFireEmission != 0u ? FluidFireSoftKnee(fireRadiance) : 0.0f;
    if (gDisplayMode == 2u)
        return float4(alpha.xxx, 1.0f);
    if (gDisplayMode == 1u)
    {
        const float3 outputColor = color + fireColor;
        const float3 previewRadiance = gFireEmission != 0u
            ? saturate(outputColor * gExposure) / max(gExposure, 1.0e-3f)
            : color * gExposure;
        const float3 composite = saturate(previewRadiance + background * transmittance);
        return float4(LinearToSRGB(composite), 1.0f);
    }
    return float4(color + fireColor, alpha);
}
