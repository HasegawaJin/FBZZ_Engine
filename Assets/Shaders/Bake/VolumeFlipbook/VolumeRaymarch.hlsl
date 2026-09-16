/// @file    VolumeRaymarch.hlsl
/// @brief   Volume Flipbook Baker: ボリュームを平行投影でレイマーチし、色と画面空間速度を横並びに書く。
/// @author  Hasegawa Jin
/// @date    2026-09-11
//
// RT は 4·tile × tile に 4 枚のタイルを横に並べる:
//   0 = 事前乗算のリニア HDR 色 (a = 1 - 透過率)
//       gDistortion なら (覆い付きの平均速度 [右, 上], 0, 1 - 透過率)。タイル 1〜3 は描かない
//   1 = (画面空間速度 [タイル UV/秒], 重みの合計, 1)
//   2 = 6 方向ライトマップ Positive (右, 上, 奥, α)       … gSixWay が 0 なら描かない
//   3 = 6 方向ライトマップ Negative (左, 下, 手前, 発光マスク)
// 6 方向マップの規約は Engine/Asset/SixWayLighting.hpp (値は «その向きから単位の白色光が来たときの
// 明るさ»。モノクロ・リニア・ストレート)。
// WHY 1 枚に並べるか: CPU への読み戻し (CaptureRenderTargetToLinearRGBA) は color 0 しか読めない。
//     MRT にするとバックエンドの改修が要り、RT を 2 枚にすると GPU 待ちが 2 回になる。
//
// 媒質は煙と液体の 2 種類が混ざる (A = 液体の割合)。煙は散乱する霧として、液体は密度が
// gLiquidThreshold を跨ぐところを表面とみなし、密度の勾配を法線にして陰影を付ける。
#include "Common/Fullscreen.hlsli"
#include "Rendering/ParticleNoise.hlsli" // FbmNoise3D

cbuffer VolumeRaymarchConstants : register(b0)
{
    float3 gCamRight;   float gHalfExtent;
    float3 gCamUp;      uint  gTileSize;
    float3 gCamForward; uint  gRaySteps;
    float3 gToLight;    uint  gShadowSteps;
    float3 gLightColor; float gExtinction;
    float3 gAmbient;    float gEmissionIntensity;
    uint  gDisplayMode;       // 0 = ベイク用の生値 / 1 = プレビュー (色) / 2 = プレビュー (α)
    float gAnisotropy;
    float gPreviewMotionScale;
    uint  gBackground;        // プレビューの背景。0 = 暗 / 1 = 明 / 2 = チェッカー
    float gExposure;
    float gLiquidThreshold;
    float gLiquidSoftness;
    float gLiquidExtinction;
    float gLiquidSpecular;
    float gLiquidGloss;
    float gLiquidF0;
    float gVoxelSize;         // 1 ボクセルの幅 [bake 単位]
    float4 gAlbedoRamp[4];    // rgb = 色 / w = 位置 (昇順)
    float4 gEmissionRamp[4];
    uint  gSixWay;            // 1 = タイル 2 / 3 に 6 方向ライトマップを描く
    uint  gOctaves;           // 多重散乱の段数 (1 = 単散乱)
    uint  gBlackbody;         // 1 = 発光の色を黒体放射で決める
    uint  gFrameIndex;        // 標本位置のずれをコマごとに変える
    float gSkyOcclusion;      // 環境光を上の煙が遮る割合
    float gDetailStrength;    // 格子より細かい起伏の強さ (0 で無効)
    float gDetailScale;
    float gDetailPeriod;      // 起伏を流れに乗せて入れ替える周期 [秒]
    float gTime;              // このコマの時刻 [秒]
    float gKelvinMin;         // 黒体放射の色の下限 (これより冷たい所も色はこの温度)
    float gKelvinMax;         // 温度 1 に当たる色温度
    uint  gDistortion;        // 1 = 色の代わりに歪みマップの生値を描く
    float gDistortionScale;   // プレビューの符号化にだけ使う (焼きの符号化は CPU)
    float gDistortionPad0;
    float gDistortionPad1;
    float gDistortionPad2;
};

Texture3D<float4> gMedium      : register(t0); // R 密度 / G 温度 / B colorKey / A 液体の割合
Texture3D<float4> gVelocity    : register(t1);
SamplerState      gLinearClamp : register(s2);

// 背景は «表示の色» (sRGB) で決め、合成のためにリニアへ戻す。
float3 PreviewBackground(float2 local)
{
    if (gBackground == 1u) return pow(float3(0.82f, 0.82f, 0.84f), 2.2f);
    if (gBackground == 2u)
    {
        const uint2 cell = uint2(local / 16.0f);
        return pow(((cell.x + cell.y) & 1u) != 0u ? float3(0.42f, 0.42f, 0.42f) : float3(0.26f, 0.26f, 0.26f), 2.2f);
    }
    return pow(float3(0.18f, 0.18f, 0.2f), 2.2f);
}

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

bool IntersectUnitBox(float3 origin, float3 direction, out float tNear, out float tFar)
{
    // 軸に平行な成分を 0 のまま割ると、原点が面上にある画素で 0·inf = NaN が出る。
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

// 引く位置そのものをノイズで数ボクセルずらす (domain warp)。
//
// WHY 濃さを揺らすだけでは足りないか: DetailFactor は密度を掛けるだけなので «輪郭» が動かない。
//     塊の形は格子のままで、表面に模様が乗っただけに見える。座標を歪めると輪郭が波打つので、
//     volume_resolution を上げずに «細かい» 絵になる (焼き時間も増えない)。
// WHY 歪みは視線側だけか: 影の行進は視線 1 標本につき shadow_steps 回走る。ここでノイズを
//     3 回引くとコストが shadow_steps 倍で跳ね返る。影側は 1 オクターブの濃淡だけを足して
//     «細部が光を遮る» ことを成立させている (OpticalDepthAlong を参照)。
// WHY 専用の定数を作らず detailStrength から導くか: 定数バッファの並びを変えずに済ませるため
//     (レイアウトを動かすと C++ の static_assert と Script DLL の再ビルドまで波及する)。
float3 WarpForDetail(float3 position)
{
    if (gDetailStrength <= 0.0f) return position;
    const float3 q = position * gDetailScale;
    const float3 offset = float3(ValueNoise3D(q), ValueNoise3D(q + 31.7f), ValueNoise3D(q + 71.3f));
    // 2 ボクセルぶんを上限にする。これ以上ずらすと «別の場所の煙» を引いて形が崩れる。
    return position + offset * (gDetailStrength * 2.0f * gVoxelSize);
}

// 画素ごとの標本位置のずれ [0,1)。固定の 0.5 だと、行進の刻みの境目が等高線の縞として残る。
static float gPixelJitter = 0.5f;

// Interleaved Gradient Noise (Jimenez 2014)。隣の画素と相関が低く、縮めたときに縞が消える。
float InterleavedGradientNoise(float2 pixel)
{
    return frac(52.9829189f * frac(dot(pixel, float2(0.06711056f, 0.00583715f))));
}

// 格子より細かい起伏。ノイズの座標を «その場の速度» で 2 層ずらし、半周期ずらして混ぜる
// (Neyret 2003, Advected Textures)。流れと一緒に動くので、止まったノイズが煙の上を滑らない。
float DetailFactor(float3 position)
{
    if (gDetailStrength <= 0.0f) return 1.0f;
    const float3 v = gVelocity.SampleLevel(gLinearClamp, position * 0.5f + 0.5f, 0.0f).xyz;
    const float period = max(gDetailPeriod, 0.05f);
    const float phase0 = frac(gTime / period);
    const float phase1 = frac(gTime / period + 0.5f);
    const float weight0 = 1.0f - abs(2.0f * phase0 - 1.0f);
    const float weight1 = 1.0f - weight0;
    const float n0 = FbmNoise3D((position - v * (phase0 * period)) * gDetailScale, 4);
    const float n1 = FbmNoise3D((position - v * (phase1 * period)) * gDetailScale + 17.31f, 4);
    // 2 層を重みで混ぜるとノイズの振幅が縮む (中間で最大 1/√2)。戻さないと detail_period の半分の
    // 周期でディテールのコントラストが脈打つ (FluidBaker.cpp の detailNormalize と同じ補正)。
    const float blended = (weight0 * n0 + weight1 * n1)
                        * rsqrt(max(weight0 * weight0 + weight1 * weight1, 1.0e-4f));
    // 上を saturate で切ると正の山だけが 1.0 で潰れ、密度を削る方向にしか効かなくなる。下だけ止める。
    //
    // WHY 振幅で割るか: 2D (FluidBaker.cpp の DetailNoise) は value/total で ±1 に正規化してから
    //     detailStrength を掛ける。ここが «× 2» のままだと同じ項目名で効き方が 1.9 倍違い、
    //     2D と 3D を行き来するたびに勘が狂う。±1 に揃えて «1.0 = 密度 ±100 %» の意味にする。
    const float kFbmAmplitude = 0.9375f; // 0.5 + 0.25 + 0.125 + 0.0625 (4 オクターブの振幅和)
    return max(1.0f + gDetailStrength * (blended / kFbmAmplitude), 0.0f);
}

// 視線の行進で使う媒質。ここは «輪郭の歪み + 流れに乗せた濃淡» の両方を足す (影側は濃淡だけ)。
// 液体には掛けない: 密度の等値面が液面なので、ノイズで削ると液面が虫食いになる。
float4 ViewMediumAt(float3 position)
{
    // 液体 (medium.a >= 0.5) は等値面で表面を出すので歪めない。歪めると液面が泡立って見える。
    const float4 straight = MediumAt(position);
    if (straight.a >= 0.5f) return straight;
    float4 medium = MediumAt(WarpForDetail(position));
    medium.a = straight.a;
    if (medium.r > 1.0e-5f) medium.r *= DetailFactor(position);
    return medium;
}

// VolumeFlipbookBaker.cpp の EvaluateVolumeRamp と同じ。位置は CPU 側で昇順に揃えてある。
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

// x = 煙の消散係数 / y = 液体の消散係数。視線の行進と影の行進で同じ式を使う。
float2 Extinctions(float4 medium)
{
    const float liquid = saturate(medium.a);
    return float2(medium.r * (1.0f - liquid) * gExtinction,
                  LiquidCoverage(medium.r) * liquid * gLiquidExtinction);
}

// toLight の向きへの光学的厚さ (∫σ ds)。影を落とすのは同じボリュームだけ。
float OpticalDepthAlong(float3 position, float3 toLight)
{
    float tNear, tFar;
    if (!IntersectUnitBox(position, toLight, tNear, tFar))
        return 0.0f;
    const float stepLength = tFar / float(max(gShadowSteps, 1u));
    float opticalDepth = 0.0f;
    [loop] for (uint i = 0; i < gShadowSteps; ++i)
    {
        // 細部を «遮る側» にも入れる。素の格子だけで測ると、視線側で足した起伏に陰影が付かず
        // 平面的な模様に見える。
        //
        // WHY 1 オクターブの濃淡で、視線側と同じ歪み + fbm を使わないか: この行は視線 1 標本につき
        //     shadow_steps 回走る。視線側と同じ厚さ (ノイズ 11 回) にすると焼き時間が桁で跳ねる。
        //     遮光に効くのは «濃いか薄いか» なので、1 回のノイズで足りる。
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

// 4π を掛けて等方散乱が 1 になるよう正規化した Henyey-Greenstein。
float PhaseHG(float cosTheta, float g)
{
    const float denominator = 1.0f + g * g - 2.0f * g * cosTheta;
    return (1.0f - g * g) / pow(max(denominator, 1.0e-4f), 1.5f);
}

// 多重散乱の近似 (Wrenninge et al. 2013, "Oz: The Great and Volumetric")。
// 消散を a^i・寄与を b^i・位相の偏りを c^i で弱めた «光の段» を重ねる (a = b = c = 0.5)。
// 単散乱だけだと煙の奥が真っ黒に落ち、実物の «内側から明るい» 柔らかさが出ない。
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

// 6 方向マップと天空光の遮蔽用。位相を掛けず、段の重みの和で割って [0,1] に保つ
// (マップは «単位の光への応答» なので 1 を超えると 8bit で飽和する)。
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

// 環境光のうち «上に積もった煙» を抜けて届く割合。煙の下側が暗くなり、塊に重さが出る。
float SkyVisibility(float3 position)
{
    if (gSkyOcclusion <= 0.0f) return 1.0f;
    return lerp(1.0f, MultiScatterIsotropic(OpticalDepthAlong(position, float3(0.0f, 1.0f, 0.0f))), gSkyOcclusion);
}

// 色温度 → リニア sRGB の色み (最大成分 = 1)。Engine/Renderer/ColorTemperature.hpp の写し (Krystek 近似)。
float3 BlackbodyColor(float kelvin)
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

// 温度 (0..1) → 発光。黒体放射なら輝度は温度の 4 乗 (Stefan-Boltzmann) で、芯だけが白く光る。
float3 EmissionAt(float temperature)
{
    const float t = saturate(temperature);
    if (gBlackbody != 0u)
        return gEmissionIntensity * (t * t * t * t) * BlackbodyColor(max(gKelvinMax * t, gKelvinMin));
    return gEmissionIntensity * t * t
        * EvaluateRamp(gEmissionRamp[0], gEmissionRamp[1], gEmissionRamp[2], gEmissionRamp[3], t);
}

// 6 方向マップの発光マスク。EmissionAt の輝度の形だけを取り出したもの。
float EmissionMask(float temperature)
{
    const float t = saturate(temperature);
    return gBlackbody != 0u ? t * t * t * t : t * t;
}

float3 LiquidNormal(float3 position)
{
    const float h = gVoxelSize;
    const float3 gradient = float3(
        MediumAt(position + float3(h, 0.0f, 0.0f)).r - MediumAt(position - float3(h, 0.0f, 0.0f)).r,
        MediumAt(position + float3(0.0f, h, 0.0f)).r - MediumAt(position - float3(0.0f, h, 0.0f)).r,
        MediumAt(position + float3(0.0f, 0.0f, h)).r - MediumAt(position - float3(0.0f, 0.0f, h)).r);
    // 密度が減る向きが外側。
    return dot(gradient, gradient) > 1.0e-10f ? -normalize(gradient) : -gCamForward;
}

float3 ShadeLiquid(float3 position, float3 albedo)
{
    const float3 n = LiquidNormal(position);
    const float3 v = -gCamForward;
    const float3 h = normalize(gToLight + v);
    // 表面から少し浮かせて影を測る。表面より内側の標本から測ると、自分の中身で真っ暗になる。
    const float shadow = LightTransmittance(position + n * (gVoxelSize * 1.5f));
    // 回り込み (wrap)。液滴は小さく、裏から光が抜けるので影側を真っ黒にしない。
    const float diffuse = saturate((dot(n, gToLight) + 0.35f) / 1.35f);
    const float fresnel = gLiquidF0 + (1.0f - gLiquidF0) * pow(1.0f - saturate(dot(n, v)), 5.0f);
    const float highlight = gLiquidSpecular * pow(saturate(dot(n, h)), max(gLiquidGloss, 1.0f));
    const float3 body = albedo * (gLightColor * diffuse * shadow + gAmbient) * (1.0f - fresnel);
    // ベイク空間に空は無いので、映り込みは環境光の色で近似する。
    const float3 reflection = fresnel * gAmbient * 2.0f + gLightColor * highlight * shadow;
    return body + reflection;
}

float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    const uint2 pixel = uint2(p.svPosition.xy);
    const uint tile = min(pixel.x / max(gTileSize, 1u), 3u);
    const bool motionHalf = tile == 1u;
    const bool sixWayTile = tile >= 2u;
    const float2 local = float2(pixel.x - tile * gTileSize, pixel.y) + 0.5f;
    if ((sixWayTile && gSixWay == 0u) || (tile != 0u && gDistortion != 0u))
        return gDisplayMode != 0u ? float4(pow(PreviewBackground(local), 1.0f / 2.2f), 1.0f) : 0.0f;
    gPixelJitter = InterleavedGradientNoise(float2(pixel) + float(gFrameIndex) * 5.588238f);
    const float2 screen = float2(local.x / float(gTileSize) * 2.0f - 1.0f,
                                 1.0f - local.y / float(gTileSize) * 2.0f);

    const float3 origin = (gCamRight * screen.x + gCamUp * screen.y) * gHalfExtent - gCamForward * 4.0f;
    float tNear, tFar;
    float3 color = 0.0f;
    float transmittance = 1.0f;
    float weightSum = 0.0f;
    float2 motionSum = 0.0f;
    float2 distortionSum = 0.0f;
    // 6 方向: このタイルが持つ 3 軸 (Positive = +右/+上/+奥、Negative = その逆) の明るさと発光。
    float3 lightSum = 0.0f;
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
            if (medium.r <= 1.0e-5f)
                continue;
            const float2 sigma = Extinctions(medium);
            const float sigmaSum = sigma.x + sigma.y;
            if (sigmaSum <= 1.0e-6f)
                continue;
            const float sampleTransmittance = exp(-sigmaSum * stepLength);
            // この標本が画素へ寄与する割合。速度の重みも同じ量を使う (見えている媒質の動きを取る)。
            const float weight = transmittance * (1.0f - sampleTransmittance);

            if (motionHalf)
            {
                const float3 v = gVelocity.SampleLevel(gLinearClamp, position * 0.5f + 0.5f, 0.0f).xyz;
                // 画像は +V が下なので up 成分の符号を反転する。タイルは 2·halfExtent を覆う。
                motionSum += weight * float2(dot(v, gCamRight), -dot(v, gCamUp)) / (2.0f * gHalfExtent);
            }
            else if (sixWayTile)
            {
                // 6 方向マップは «明るさ» だけを持つ。色 (albedo の色味・光の色) はランタイムの粒子色と
                // 光源が持つので、ここでは反射率の輝度だけを掛ける。
                // 位相関数は掛けない: 視線と光の角度はランタイムにしか分からない。
                const float3 albedo = EvaluateRamp(gAlbedoRamp[0], gAlbedoRamp[1], gAlbedoRamp[2], gAlbedoRamp[3],
                                                   medium.b);
                const float reflectance = dot(albedo, float3(0.2126f, 0.7152f, 0.0722f));
                lightSum += weight * reflectance * float3(
                    MultiScatterIsotropic(OpticalDepthAlong(position, gCamRight * axisSign)),
                    MultiScatterIsotropic(OpticalDepthAlong(position, gCamUp * axisSign)),
                    MultiScatterIsotropic(OpticalDepthAlong(position, gCamForward * axisSign)));
                emissionSum += weight * EmissionMask(medium.g);
            }
            else if (gDistortion != 0u)
            {
                // 見えている媒質の動きを画面の右・上へ投影して積む (MV と同じ重み)。陰影は要らない。
                const float3 v = gVelocity.SampleLevel(gLinearClamp, position * 0.5f + 0.5f, 0.0f).xyz;
                distortionSum += weight * float2(dot(v, gCamRight), dot(v, gCamUp));
            }
            else
            {
                const float3 albedo = EvaluateRamp(gAlbedoRamp[0], gAlbedoRamp[1], gAlbedoRamp[2], gAlbedoRamp[3],
                                                   medium.b);
                float3 radiance = 0.0f;
                if (sigma.x > 0.0f)
                    radiance += sigma.x * albedo * (gLightColor * MultiScatter(OpticalDepthAlong(position, gToLight), cosLight)
                                                   + gAmbient * SkyVisibility(position));
                if (sigma.y > 0.0f)
                    radiance += sigma.y * ShadeLiquid(position, albedo);
                radiance /= sigmaSum;

                const float temperature = saturate(medium.g);
                // 発光は «不透明な炎の輝度» として散乱と同じ不透明度で重み付けする。
                // WHY: 密度 × 距離で積むと、消散係数 k の煙からは実質 emission / k しか出てこない
                //      (既定の k = 10 で炎がほぼ見えなかった)。この形なら Emission の値がそのまま
                //      炎の芯の明るさになり、消散係数を変えても明るさがずれない。
                //      輝度が 1 を超えれば RGB > α になるので、Atlas は事前乗算で持つ。
                const float3 emission = EmissionAt(temperature);
                color += weight * (radiance + emission);
            }
            weightSum += weight;
            transmittance *= sampleTransmittance;
            if (transmittance < 1.0e-3f)
                break;
        }
    }

    const float3 background = PreviewBackground(local);
    if (sixWayTile)
    {
        // ストレートで持つ (粒子の不透明度は Positive の α が決める)。
        const float coverage = 1.0f - transmittance;
        const float inverseCoverage = 1.0f / max(coverage, 1.0e-4f);
        const float3 lightmap = saturate(lightSum * inverseCoverage);
        if (gDisplayMode != 0u)
            return float4(pow(max(lerp(background, lightmap, coverage), 0.0f), 1.0f / 2.2f), 1.0f);
        return float4(lightmap, tile == 2u ? coverage : saturate(emissionSum * inverseCoverage));
    }
    if (motionHalf)
    {
        const float2 motion = motionSum / max(weightSum, 1.0e-5f);
        if (gDisplayMode != 0u)
        {
            // 赤 = 右へ / 緑 = 下へ。静止は (0.5, 0.5) の灰色。
            const float3 visual = pow(float3(saturate(0.5f + motion * gPreviewMotionScale), 0.5f), 2.2f);
            return float4(pow(max(lerp(background, visual, saturate(weightSum)), 0.0f), 1.0f / 2.2f), 1.0f);
        }
        return float4(motion, weightSum, 1.0f);
    }

    if (gDistortion != 0u)
    {
        const float coverage = 1.0f - transmittance;
        // 焼きには符号化前の平均速度を返す。符号化 (0.5 中心・倍率・頭打ち) は CPU の EncodeVolumeDistortion が
        // 行う。WHY: ループの重ねは符号化の前の速度で混ぜないと、頭打ちの所で向きがずれる。
        const float2 velocity = distortionSum / max(weightSum, 1.0e-5f);
        if (gDisplayMode == 2u)
            return float4(coverage.xxx, 1.0f);
        if (gDisplayMode == 1u)
        {
            // EncodeVolumeDistortion の写し (画像は +V が下。速さ 1 で頭打ち → 倍率 → 長さ 0.5 の安全網)。
            const float speed = length(velocity);
            const float gain = speed > 1.0e-5f ? min(speed, 1.0f) / speed * gDistortionScale : 0.0f;
            float2 d = float2(velocity.x, -velocity.y) * gain;
            const float len = length(d);
            if (len > 0.5f)
                d *= 0.5f / len;
            const float3 encoded = float3(saturate(0.5f + d), 0.5f);
            return float4(lerp(pow(background, 1.0f / 2.2f), encoded, coverage), 1.0f);
        }
        return float4(velocity, 0.0f, coverage);
    }

    const float alpha = 1.0f - transmittance;
    if (gDisplayMode == 2u)
        return float4(alpha.xxx, 1.0f);
    if (gDisplayMode == 1u)
    {
        const float3 composite = saturate(color * gExposure + background * transmittance);
        return float4(pow(composite, 1.0f / 2.2f), 1.0f);
    }
    return float4(color, alpha);
}
