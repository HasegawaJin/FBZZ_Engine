/**
 * @file    CloudVolume.hlsli
 * @brief   ボリューメトリック雲の密度場を雲本体と光芒の両方から同じ式で引くための共有定義
 * @author  Hasegawa Jin
 * @date    2026-08-22
 *
 * WHY このファイルが要るか:
 *   雲の隙間から差す光の線は「空気中のサンプル点から見て太陽が雲に遮られているか」で決まる。
 *   遮蔽判定に使う密度が雲本体の描画と一致していないと、影の帯が雲の位置とずれて
 *   「無い雲の影が落ちている」絵になる。密度の定義を 1 か所に置いて両者を必ず一致させる。
 */
#ifndef FBZZ_CLOUD_VOLUME_HLSLI
#define FBZZ_CLOUD_VOLUME_HLSLI

// CB_MATERIAL を雲パラメータで占有するため、Constants.hlsli の既定 MaterialConstants を抑止する。
// 本ファイルが Constants.hlsli を include するので、include 側は順序を気にしなくてよい。
#ifndef FBZZ_MATERIAL_CONSTANTS
#define FBZZ_MATERIAL_CONSTANTS
#endif
#include "Common/Constants.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX3D_T(float4, g_cloudShape, TEX_CLOUD_SHAPE_SLOT);   // R=Perlin-Worley, GBA=Worley FBM 帯
FBZZ_TEX3D_T(float4, g_cloudDetail, TEX_CLOUD_DETAIL_SLOT);  // RGB=高周波 Worley FBM
SamplerState      sampCloudNoise : register(SAMPLER_WRAP_LINEAR);

cbuffer VolumetricCloudConstants : register(CB_MATERIAL)
{
    float4 cloudLayer;    // x=bottom, y=top, z=density, w=coverage
    float4 cloudNoise;    // x=1/cloudSize, y=detail 倍率, z=time, w=maxDistance
    float4 cloudWind;     // xz=windDir, y=windSpeed, w=stepCount
    float4 cloudLighting; // x=absorption, y=ambientStrength, z=silverLining, w=lightShaftStrength
    float4 cloudAlbedo;   // rgb=albedo, w=ambientGradient
    float4 cloudWeather;  // x=1/weatherSize, y=weatherAmount, z=detailStrength, w=evolutionSpeed
    float4 cloudShading;  // x=extinction, y=sunIntensity, z=powder, w=multiScatter
    float4 cloudProfile;  // x=bottomSoftness, y=topSoftness, z=anisotropy, w=lightSteps
    float4 cloudRange;    // x=minDistance, y=fadeDistance, z=horizonFade, w=未使用
    float4 cloudSunTint;  // rgb=太陽側の色
    float4 cloudAmbTint;  // rgb=影側の色
};

// 光芒の遮蔽判定で雲層を刺す枚数。体積光の 1 ステップごとに丸ごと走るため、
// 増やすと 3D テクスチャのフェッチ数が (画素 × ステップ数 × これ) で効いてくる。
static const int   kCloudShaftTaps     = 2;
// 太陽が寝たときの光路長の上限 (雲層の厚み比)。無制限だと雲影が画面全体を覆う板になる。
static const float kCloudShaftMaxSlant = 20.0f;
// 雲量に応じて Shape ボリュームの読み取りスライスをずらす幅 (ノイズ空間)。
static const float kCloudSliceSpread   = 0.8f;

float FBZZCloudRemap(float v, float lo, float hi, float nlo, float nhi)
{
    return nlo + saturate((v - lo) / max(hi - lo, 1e-5f)) * (nhi - nlo);
}

float FBZZCloudExtinction() { return max(cloudShading.x, 1e-4f); }

// 雲層内の高さ (0=底, 1=天) に対する密度プロファイル。
// bottomSoftness を上げると底が丸く、topSoftness を上げると天がなだらかに散る。
float FBZZCloudHeightGradient(float h)
{
    float bottomSoft = clamp(cloudProfile.x, 0.01f, 0.9f);
    float topSoft    = clamp(cloudProfile.y, 0.01f, 0.9f);
    return smoothstep(0.0f, bottomSoft, h) * (1.0f - smoothstep(1.0f - topSoft, 1.0f, h));
}

// 雲全体を流すワールド XZ オフセット。ステップ毎に変わらないのでループ前に一度だけ求める。
float2 FBZZCloudWindOffset()
{
    float2 raw = float2(cloudWind.x, cloudWind.z);
    float  len = length(raw);
    float2 dir = len > 1.0e-4f ? raw / len : float2(1.0f, 0.0f);
    return dir * (cloudWind.y * cloudNoise.z);
}

// FBZZCloudWeather — 雲量の大域的なムラ (weather map 相当)。1.0 で「その柱は雲量そのまま」。
//
// WHY これが要るか:
//   Shape ボリュームは 128³ のタイラブルテクスチャなので、1/shapeScale ワールド単位ごとに
//   まったく同じ雲が格子状に並ぶ。既定値なら 1000 単位ほどで、視界内に同じ塊が何個も現れて
//   「箱の中の雲が連続している」ように見える。周期の 1 桁大きい雲量フィールドを掛けると、
//   どの格子が濃くどこが晴れるかがバラけて、繰り返しが視覚的に解ける。
float FBZZCloudWeather(float3 wp, float2 windWorld)
{
    float amount = saturate(cloudWeather.y);
    if (amount <= 0.0f) return 1.0f;

    // 高さは使わない (雲量は柱ごとに決まる)。大きな塊は本体より遅く流す。
    float3 sp = float3(wp.x + windWorld.x * 0.35f, cloudLayer.x, wp.z + windWorld.y * 0.35f)
              * max(cloudWeather.x, 1e-7f);
    float2 w = g_cloudShape.SampleLevel(sampCloudNoise, sp, 0.0f).rg;
    float field = saturate(w.r * 0.65f + w.g * 0.35f);
    // amount=0 で従来どおり一様。上げるほど晴れ間と厚い塊の差が開く。
    return lerp(1.0f, field * 1.7f, amount);
}

// Shape のみの密度 (0..1)。weather は FBZZCloudWeather の結果を呼び出し側で使い回す。
// WHY 引数で受ける: ライトマーチや光芒の遮蔽は数ワールド単位しか動かないので雲量はほぼ一定。
//     ステップごとに引き直すと 3D フェッチが倍になるだけで絵は変わらない。
float FBZZCloudShape01(float3 wp, float2 windWorld, float weather)
{
    float h = saturate((wp.y - cloudLayer.x) / max(cloudLayer.y - cloudLayer.x, 1.0f));
    float grad = FBZZCloudHeightGradient(h);
    if (grad <= 0.0f) return 0.0f;

    // xz は風で流し、y は時間でゆっくり進化させる。
    float3 sp = (wp + float3(windWorld.x, cloudNoise.z * cloudWeather.w, windWorld.y)) * cloudNoise.x;
    // 地域ごとにボリュームの別スライスを読み、同じ雲が繰り返して見えるのをさらに崩す。
    // WHY: 雲量ムラだけでは「濃い/薄い」しか変わらず、シルエットは同じ塊が並んだままになる。
    //      深さ方向をずらすと、雲量が同じ領域どうしでも別の形の雲になる。weather=1
    //      (Weather Amount = 0) では 0 になるので、一様な旧挙動もそのまま再現できる。
    sp.y += (weather - 1.0f) * kCloudSliceSpread;
    // WHY: 可変回数のレイマーチ内では暗黙微分が未定義になるため、LOD 0 を明示する。
    float4 shape = g_cloudShape.SampleLevel(sampCloudNoise, sp, 0.0f);
    float lowFreq = shape.g * 0.625f + shape.b * 0.25f + shape.a * 0.125f;
    float base = FBZZCloudRemap(shape.r, lowFreq - 1.0f, 1.0f, 0.0f, 1.0f) * grad;

    float coverage = saturate(cloudLayer.w * weather);
    return FBZZCloudRemap(base, 1.0f - coverage, 1.0f, 0.0f, 1.0f);
}

// Detail (高周波 Worley) で縁を侵食して綿のようなウィスプを作る。
float FBZZCloudErodeDetail(float base01, float3 wp, float2 windWorld)
{
    float morph = max(cloudWeather.z, 0.0f);
    if (base01 <= 0.0f || morph <= 0.0f) return base01;
    float3 dp = (wp + float3(windWorld.x * 2.0f, 0.0f, windWorld.y * 2.0f))
              * cloudNoise.x * cloudNoise.y;
    float3 det = g_cloudDetail.SampleLevel(sampCloudNoise, dp, 0.0f).rgb;
    float detFbm = det.r * 0.625f + det.g * 0.25f + det.b * 0.125f;
    return saturate(base01 - detFbm * morph * (1.0f - base01));
}

// レイと雲層スラブの交差区間 [t0, t1]。t1 は maxDistance で頭打ちにしない (呼び出し側の責務)。
bool FBZZCloudSlabIntersect(float3 ro, float3 rd, out float t0, out float t1)
{
    if (abs(rd.y) < 1e-4f)
    {
        if (ro.y < cloudLayer.x || ro.y > cloudLayer.y) { t0 = 0.0f; t1 = 0.0f; return false; }
        t0 = 0.0f; t1 = cloudNoise.w; return true;
    }
    float tb = (cloudLayer.x - ro.y) / rd.y;
    float tt = (cloudLayer.y - ro.y) / rd.y;
    t0 = max(min(tb, tt), 0.0f);
    t1 = max(tb, tt);
    return t1 > t0;
}

// 雲の中を太陽方向へマーチして得る光学的深さ。exp(-これ) が到達光の透過率になる。
float FBZZCloudOpticalDepthToSun(float3 wp, float3 sunDir, float2 windWorld, float weather)
{
    int   steps   = clamp((int)cloudProfile.w, 1, 8);
    float stepLen = max((cloudLayer.y - cloudLayer.x) / float(steps), 1.0f);
    float density = max(cloudLayer.z, 0.0f);
    float optical = 0.0f;
    [loop]
    for (int k = 0; k < steps; ++k)
    {
        float3 lp = wp + sunDir * (float(k) + 0.5f) * stepLen;
        optical += FBZZCloudShape01(lp, windWorld, weather) * density * stepLen;
    }
    return optical * FBZZCloudExtinction() * max(cloudLighting.x, 0.0f);
}

// FBZZCloudShaftTransmittance — 雲より下にある大気サンプル点 wp から見た太陽の透過率。
// 光芒 (ゴッドレイ) をこれで減光すると、雲の切れ間の形がそのまま光の線になる。
//
// WHY 交差区間をマーチせず「雲層を数枚の水平面で刺す」か:
//   体積光は 1 ピクセルあたり数十ステップ走る。その各ステップで太陽レイを本気で
//   マーチすると 3D テクスチャのフェッチ数が二桁増えて実用にならない。雲層は薄い板なので、
//   太陽レイが層内で通る距離 (thickness / sunDir.y) を光路長として与え、
//   層内の数枚の高さで密度を拾えば、影の「形」は十分に再現できる。
float FBZZCloudShaftTransmittance(float3 wp, float3 sunDir, float2 windWorld)
{
    float shaft = saturate(cloudLighting.w);
    // 太陽が地平線を割ると交点が発散するので、そこだけは打ち切る。
    if (shaft <= 0.0f || sunDir.y < 0.02f || wp.y >= cloudLayer.y) return 1.0f;

    float thickness = max(cloudLayer.y - cloudLayer.x, 1.0f);
    // 斜めに差し込むほど雲の中を長く通る。夕方の長い光芒はこの伸びが作る。
    float slant     = min(thickness / sunDir.y, thickness * kCloudShaftMaxSlant);
    float density   = max(cloudLayer.z, 0.0f);

    float3 firstHit = wp + sunDir * max((cloudLayer.x - wp.y) / sunDir.y, 0.0f);
    float  weather  = FBZZCloudWeather(firstHit, windWorld);

    float optical = 0.0f;
    [unroll]
    for (int k = 0; k < kCloudShaftTaps; ++k)
    {
        float planeY = lerp(cloudLayer.x, cloudLayer.y, (float(k) + 0.5f) / float(kCloudShaftTaps));
        float3 hit = wp + sunDir * max((planeY - wp.y) / sunDir.y, 0.0f);
        optical += FBZZCloudShape01(hit, windWorld, weather);
    }
    optical *= density * (slant / float(kCloudShaftTaps))
             * FBZZCloudExtinction() * max(cloudLighting.x, 0.0f);

    return lerp(1.0f, exp(-optical), shaft);
}

#endif // FBZZ_CLOUD_VOLUME_HLSLI
