/// @file    CloudVolume.hlsli
/// @brief   ボリューメトリック雲の密度場を雲本体と光芒の両方から同じ式で引くための共有定義
/// @author  Hasegawa Jin
/// @date    2026-08-22
/// @note 雲と光芒が同じ密度場をサンプルすることで、太陽を遮る雲と影の位置を合わせる。
#ifndef FBZZ_CLOUD_VOLUME_HLSLI
#define FBZZ_CLOUD_VOLUME_HLSLI

/// @note CB_MATERIAL を雲パラメータで占有するため、Constants.hlsli の既定 MaterialConstants を抑止する。
/// @note 本ファイルが Constants.hlsli を include するので、include 側は順序を気にしなくてよい。
#ifndef FBZZ_MATERIAL_CONSTANTS
#define FBZZ_MATERIAL_CONSTANTS
#endif
#include "Common/Constants.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX3D_T(float4, g_cloudShape, TEX_CLOUD_SHAPE_SLOT);   /// @note R=Perlin-Worley, GBA=Worley FBM 帯
FBZZ_TEX3D_T(float4, g_cloudDetail, TEX_CLOUD_DETAIL_SLOT);  /// @note RGB=高周波 Worley FBM
SamplerState      sampCloudNoise : register(SAMPLER_WRAP_LINEAR);

cbuffer VolumetricCloudConstants : register(FBZZ_REGISTER(b, CB_VOLUMETRIC_CLOUD_SLOT))
{
    float4 cloudLayer;    /// @note x=bottom, y=top, z=density, w=coverage
    float4 cloudNoise;    /// @note x=1/cloudSize, y=detail 倍率, z=time, w=maxDistance
    float4 cloudWind;     /// @note xz=windDir, y=windSpeed, w=stepCount
    float4 cloudLighting; /// @note x=absorption, y=ambientStrength, z=silverLining, w=lightShaftStrength
    float4 cloudAlbedo;   /// @note rgb=albedo, w=ambientGradient
    float4 cloudWeather;  /// @note x=1/weatherSize, y=weatherAmount, z=detailStrength, w=evolutionSpeed
    float4 cloudShading;  /// @note x=extinction, y=sunIntensity, z=powder, w=multiScatter
    float4 cloudProfile;  /// @note x=bottomSoftness, y=topSoftness, z=anisotropy, w=lightSteps
    float4 cloudRange;    /// @note x=minDistance, y=fadeDistance, z=horizonFade, w=未使用
    float4 cloudSunTint;  /// @note rgb=太陽側の色
    float4 cloudAmbTint;  /// @note rgb=影側の色
};

/// @note 光芒の遮蔽判定で雲層を刺す枚数。体積光の 1 ステップごとに丸ごと走るため、
/// @note 増やすと 3D テクスチャのフェッチ数が (画素 × ステップ数 × これ) で効いてくる。
static const int   kCloudShaftTaps     = 2;
/// @note 太陽が寝たときの光路長の上限 (雲層の厚み比)。無制限だと雲影が画面全体を覆う板になる。
static const float kCloudShaftMaxSlant = 20.0f;
/// @note 雲量に応じて Shape ボリュームの読み取りスライスをずらす幅 (ノイズ空間)。
static const float kCloudSliceSpread   = 0.8f;

float FBZZCloudRemap(float v, float lo, float hi, float nlo, float nhi)
{
    return nlo + saturate((v - lo) / max(hi - lo, 1e-5f)) * (nhi - nlo);
}

float FBZZCloudExtinction() { return max(cloudShading.x, 1e-4f); }

/// @note 雲層内の高さ (0=底, 1=天) に対する密度プロファイル。
/// @note bottomSoftness を上げると底が丸く、topSoftness を上げると天がなだらかに散る。
float FBZZCloudHeightGradient(float h)
{
    float bottomSoft = clamp(cloudProfile.x, 0.01f, 0.9f);
    float topSoft    = clamp(cloudProfile.y, 0.01f, 0.9f);
    return smoothstep(0.0f, bottomSoft, h) * (1.0f - smoothstep(1.0f - topSoft, 1.0f, h));
}

/// @note 雲全体を流すワールド XZ オフセット。ステップ毎に変わらないのでループ前に一度だけ求める。
float2 FBZZCloudWindOffset()
{
    float2 raw = float2(cloudWind.x, cloudWind.z);
    float  len = length(raw);
    float2 dir = len > 1.0e-4f ? raw / len : float2(1.0f, 0.0f);
    return dir * (cloudWind.y * cloudNoise.z);
}

/// @note FBZZCloudWeather — 雲量の大域的なムラ (weather map 相当)。1.0 で「その柱は雲量そのまま」。
///
/// @note これが要るか:
/// @note Shape ボリュームは 128³ のタイラブルテクスチャなので、1/shapeScale ワールド単位ごとに
/// @note まったく同じ雲が格子状に並ぶ。既定値なら 1000 単位ほどで、視界内に同じ塊が何個も現れて
/// @note 「箱の中の雲が連続している」ように見える。周期の 1 桁大きい雲量フィールドを掛けると、
/// @note どの格子が濃くどこが晴れるかがバラけて、繰り返しが視覚的に解ける。
float FBZZCloudWeather(float3 wp, float2 windWorld)
{
    float amount = saturate(cloudWeather.y);
    if (amount <= 0.0f) return 1.0f;

    /// @note 高さは使わない (雲量は柱ごとに決まる)。大きな塊は本体より遅く流す。
    float3 sp = float3(wp.x + windWorld.x * 0.35f, cloudLayer.x, wp.z + windWorld.y * 0.35f)
              * max(cloudWeather.x, 1e-7f);
    float2 w = g_cloudShape.SampleLevel(sampCloudNoise, sp, 0.0f).rg;
    float field = saturate(w.r * 0.65f + w.g * 0.35f);
    /// @note amount=0 で従来どおり一様。上げるほど晴れ間と厚い塊の差が開く。
    return lerp(1.0f, field * 1.7f, amount);
}

/// @note Shape のみの密度 (0..1)。weather は FBZZCloudWeather の結果を呼び出し側で使い回す。
/// @note 引数で受ける: ライトマーチや光芒の遮蔽は数ワールド単位しか動かないので雲量はほぼ一定。
/// @note ステップごとに引き直すと 3D フェッチが倍になるだけで絵は変わらない。
float FBZZCloudShape01(float3 wp, float2 windWorld, float weather)
{
    float h = saturate((wp.y - cloudLayer.x) / max(cloudLayer.y - cloudLayer.x, 1.0f));
    float grad = FBZZCloudHeightGradient(h);
    if (grad <= 0.0f) return 0.0f;

    /// @note xz は風で流し、y は時間でゆっくり進化させる。
    float3 sp = (wp + float3(windWorld.x, cloudNoise.z * cloudWeather.w, windWorld.y)) * cloudNoise.x;
    /// @note 地域ごとにボリュームの別スライスを読み、同じ雲が繰り返して見えるのをさらに崩す。
    /// @note 雲量ムラだけでは「濃い/薄い」しか変わらず、シルエットは同じ塊が並んだままになる。
    /// @note 深さ方向をずらすと、雲量が同じ領域どうしでも別の形の雲になる。weather=1
    /// @note (Weather Amount = 0) では 0 になるので、一様な旧挙動もそのまま再現できる。
    sp.y += (weather - 1.0f) * kCloudSliceSpread;
    /// @note 可変回数のレイマーチ内では暗黙微分が未定義になるため、LOD 0 を明示する。
    float4 shape = g_cloudShape.SampleLevel(sampCloudNoise, sp, 0.0f);
    float lowFreq = shape.g * 0.625f + shape.b * 0.25f + shape.a * 0.125f;
    float base = FBZZCloudRemap(shape.r, lowFreq - 1.0f, 1.0f, 0.0f, 1.0f) * grad;

    float coverage = saturate(cloudLayer.w * weather);
    return FBZZCloudRemap(base, 1.0f - coverage, 1.0f, 0.0f, 1.0f);
}

/// @note Detail (高周波 Worley) で縁を侵食して綿のようなウィスプを作る。
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

/// @note レイと雲層スラブの交差区間 [t0, t1]。t1 は maxDistance で頭打ちにしない (呼び出し側の責務)。
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

/// @note 雲の中を太陽方向へマーチして得る光学的深さ。exp(-これ) が到達光の透過率になる。
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

/// @note FBZZCloudShaftTransmittance — 雲より下にある大気サンプル点 wp から見た太陽の透過率。
/// @note 光芒 (ゴッドレイ) をこれで減光すると、雲の切れ間の形がそのまま光の線になる。
///
/// @note 交差区間をマーチせず「雲層を数枚の水平面で刺す」か:
/// @note 体積光は 1 ピクセルあたり数十ステップ走る。その各ステップで太陽レイを本気で
/// @note マーチすると 3D テクスチャのフェッチ数が二桁増えて実用にならない。雲層は薄い板なので、
/// @note 太陽レイが層内で通る距離 (thickness / sunDir.y) を光路長として与え、
/// @note 層内の数枚の高さで密度を拾えば、影の「形」は十分に再現できる。
float FBZZCloudShaftTransmittance(float3 wp, float3 sunDir, float2 windWorld)
{
    float shaft = saturate(cloudLighting.w);
    /// @note 太陽が地平線を割ると交点が発散するので、そこだけは打ち切る。
    if (shaft <= 0.0f || sunDir.y < 0.02f || wp.y >= cloudLayer.y) return 1.0f;

    float thickness = max(cloudLayer.y - cloudLayer.x, 1.0f);
    /// @note 斜めに差し込むほど雲の中を長く通る。夕方の長い光芒はこの伸びが作る。
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

#endif /// @note FBZZ_CLOUD_VOLUME_HLSLI
