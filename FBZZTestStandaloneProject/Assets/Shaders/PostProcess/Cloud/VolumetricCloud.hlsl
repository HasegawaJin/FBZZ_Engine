// FBZZ Engine
// PostProcess/Cloud/VolumetricCloud.hlsl | PostProcess
// 事前ベイクした 3D ノイズ (Shape + Detail) を使う本格ボリューメトリック雲。
// 視線レイマーチ + 太陽方向ライトマーチ (セルフシャドウ) + Beer-Powder + HG 位相。
//
// WHY: 手続き型 FBM をステップ毎に回すと ALU ネックになるため、Nubis/Horizon と同様に
//      タイラブルな 3D ノイズを HW トライリニアでサンプルする。立体的な塊・パララックス・
//      光の回り込み (シルバーライニング) が安価に出せる。
#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"
#include "Platform/Backend.hlsli"

Texture2D<float>  g_depth       : register(TEX_DEPTH);
Texture3D<float4> g_cloudShape  : register(TEX_CLOUD_SHAPE);   // R=Perlin-Worley, GBA=Worley FBM 帯
Texture3D<float4> g_cloudDetail : register(TEX_CLOUD_DETAIL);  // RGB=高周波 Worley FBM
SamplerState sampDefault : register(SAMPLER_DEFAULT);     // s0: depth (clamp)
SamplerState sampNoise   : register(SAMPLER_WRAP_LINEAR); // s4: 3D ノイズ (wrap)

cbuffer VolumetricCloudConstants : register(CB_MATERIAL)
{
    float4 cloudLayer;    // x=bottom, y=top, z=density, w=coverage
    float4 cloudNoise;    // x=shapeScale, y=detailScale, z=time, w=maxDistance
    float4 cloudWind;     // xz=windDir, y=windSpeed, w=stepCount
    float4 cloudLighting; // x=absorption, y=ambient, z=silverLining, w=unused
    float4 cloudAlbedo;   // rgb=albedo
};

// ---- チューニング定数 (見た目はここと Inspector の density/coverage/absorption で詰める) ----
static const float kPI          = 3.14159265f;
static const int   kLightSteps  = 4;      // 太陽方向ライトマーチのステップ数 (6→4 で軽量化)
static const float kExtinction  = 0.03f;  // 密度→消散係数のスケール
static const float kDetailMorph = 0.35f;  // detail による縁の侵食量
static const float kPowder      = 0.7f;   // Beer-Powder の暗縁効果の強さ
static const float kEmptyStep   = 2.0f;   // 空白領域でのステップ倍率 (empty-space skip)

struct FSTriVSOut
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

FSTriVSOut VSMain(uint id : SV_VertexID)
{
    FSTriVSOut o;
    o.uv = float2((id & 1u) ? 2.0f : 0.0f,
                  (id & 2u) ? 2.0f : 0.0f);
    o.svPosition = float4(o.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

float Remap(float v, float lo, float hi, float nlo, float nhi)
{
    return nlo + saturate((v - lo) / max(hi - lo, 1e-5f)) * (nhi - nlo);
}

// 雲層内の高さ (0=底, 1=天) に対する密度プロファイル。底は丸く、天はなだらかに減衰。
float HeightGradient(float h)
{
    return smoothstep(0.0f, 0.18f, h) * (1.0f - smoothstep(0.55f, 1.0f, h));
}

float HenyeyGreenstein(float cosT, float g)
{
    float g2 = g * g;
    return (1.0f - g2) / (4.0f * kPI * pow(max(1.0f + g2 - 2.0f * g * cosT, 1e-4f), 1.5f));
}

bool IntersectCloudLayer(float3 ro, float3 rd, out float t0, out float t1)
{
    float bottom = cloudLayer.x;
    float top    = cloudLayer.y;
    if (abs(rd.y) < 1e-4f)
    {
        if (ro.y < bottom || ro.y > top) return false;
        t0 = 0.0f; t1 = cloudNoise.w; return true;
    }
    float tb = (bottom - ro.y) / rd.y;
    float tt = (top    - ro.y) / rd.y;
    t0 = max(min(tb, tt), 0.0f);
    t1 = max(tb, tt);
    return t1 > t0;
}

// Shape のみの密度 (0..1)。空白判定・ライトマーチ用の安価サンプル (Texture3D 1 回)。
float SampleShape01(float3 wp, float2 windWorld)
{
    float bottom = cloudLayer.x;
    float top    = cloudLayer.y;
    float h = saturate((wp.y - bottom) / max(top - bottom, 1.0f));
    float grad = HeightGradient(h);
    if (grad <= 0.0f) return 0.0f;

    // Shape: ワールド座標に風オフセットを足して 3D サンプル (xz=流れ, y=ゆっくり進化)。
    float3 sp = (wp + float3(windWorld.x, cloudNoise.z * 2.0f, windWorld.y)) * cloudNoise.x;
    // WHY: 可変回数のレイマーチ内では暗黙微分が未定義になるため、LOD 0 を明示する。
    float4 shape = g_cloudShape.SampleLevel(sampNoise, sp, 0.0f);
    float lowFreq = shape.g * 0.625f + shape.b * 0.25f + shape.a * 0.125f;
    float base = Remap(shape.r, lowFreq - 1.0f, 1.0f, 0.0f, 1.0f) * grad;
    // カバレッジで雲量を制御 (Remap で薄い所を削り、濃い所を残す)。
    return Remap(base, 1.0f - cloudLayer.w, 1.0f, 0.0f, 1.0f);
}

// Detail (高周波 Worley) で縁を侵食して綿のようなウィスプを作る。base01 は SampleShape01 の結果。
float ErodeDetail(float base01, float3 wp, float2 windWorld)
{
    if (base01 <= 0.0f) return 0.0f;
    float3 dp = (wp + float3(windWorld.x * 2.0f, 0.0f, windWorld.y * 2.0f)) * cloudNoise.x * cloudNoise.y;
    // WHY: Shape と同様に可変回数ループから呼ばれるため、暗黙微分を使用しない。
    float3 det = g_cloudDetail.SampleLevel(sampNoise, dp, 0.0f).rgb;
    float detFbm = det.r * 0.625f + det.g * 0.25f + det.b * 0.125f;
    return saturate(base01 - detFbm * kDetailMorph * (1.0f - base01));
}

// 太陽方向へ短くマーチして到達光の透過率を求める (セルフシャドウ)。Shape のみで軽量化。
float LightMarch(float3 wp, float3 sunDir, float2 windWorld)
{
    float stepLen = max((cloudLayer.y - cloudLayer.x) / float(kLightSteps), 1.0f);
    float density = max(cloudLayer.z, 0.0f);
    float optical = 0.0f;
    [unroll]
    for (int k = 0; k < kLightSteps; ++k)
    {
        float3 lp = wp + sunDir * (float(k) + 0.5f) * stepLen;
        optical += SampleShape01(lp, windWorld) * density * stepLen;
    }
    return exp(-optical * kExtinction * max(cloudLighting.x, 0.0f));
}

float4 PSMain(FSTriVSOut p) : SV_Target0
{
    float ndcDepth = g_depth.Sample(sampDefault, p.uv).r;
    float sceneDepth = ndcDepth >= 0.9999f
        ? cloudNoise.w
        : distance(cameraPos, ReconstructWorldPos(p.uv, ndcDepth, invViewProjection));

    float3 farPos = ReconstructWorldPos(p.uv, 1.0f, invViewProjection);
    float3 rd = normalize(farPos - cameraPos);

    float t0, t1;
    if (!IntersectCloudLayer(cameraPos, rd, t0, t1))
        return float4(0.0f, 0.0f, 0.0f, 0.0f);

    t1 = min(t1, min(sceneDepth, cloudNoise.w));
    if (t1 <= t0)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);

    int   steps   = clamp((int)cloudWind.w, 8, 96);
    float density = max(cloudLayer.z, 0.0f);
    // 細ステップは「雲層の厚み」基準で一定の細かさにする。レイ全長(t1-t0)で割ると、距離や視線角度で
    // 雲内のサンプル密度が変わり、遠景/浅い角度で崩れて高ステップが必要になっていた。厚み基準なら
    // どの視点でも雲内のサンプル数が一定になり、少ステップ(20前後)でも綺麗に保てる。
    float stepLen = max((cloudLayer.y - cloudLayer.x) / (float)steps, 0.5f);

    // ステップ内で一定の量はループ前に求める。
    float2 windDir   = normalize(float2(cloudWind.x, cloudWind.z));
    float2 windWorld = windDir * (cloudWind.y * cloudNoise.z);
    float3 sunDir    = normalize(-lightDir);
    float3 lightCol  = lightColor * max(lightIntensity, 0.0f);
    float3 ambient   = (ambientColor * 2.0f + lightCol * 0.2f) * max(cloudLighting.y, 0.0f);
    float3 albedo    = cloudAlbedo.rgb;

    // 2 ローブ HG 位相 + 太陽近傍のシルバーライニング。
    float cosT  = dot(rd, sunDir);
    float phase = max(HenyeyGreenstein(cosT, 0.2f), HenyeyGreenstein(cosT, -0.15f) * 0.6f);
    float silver = pow(saturate(cosT), 4.0f) * max(cloudLighting.z, 0.0f);

    // バンディング抑制: 開始位置をピクセルごとに [0,1)*stepLen だけずらす
    // (ハーフ解像度 + 少ステップでも縞が出ないように)。
    float dither = frac(sin(dot(p.uv, float2(12.9898f, 78.233f))) * 43758.5453f);
    float t = t0 + dither * stepLen;

    float transmittance = 1.0f;
    float3 scatter = 0.0f;

    // empty-space skip: 安価な Shape サンプルで空白を粗ステップ (×kEmptyStep) で飛ばし、
    // 雲付近のみ細ステップで detail + ライトマーチを行う。厚み基準の細ステップなので浅いレイは
    // 反復が増える → 上限を steps*4 に広げて層を貫通できるようにする (空白スキップで実コストは抑制)。
    [loop]
    for (int i = 0; i < steps * 4 && t < t1 && transmittance > 0.01f; ++i)
    {
        float3 wp = cameraPos + rd * t;

        float base = SampleShape01(wp, windWorld);
        if (base <= 0.001f) { t += stepLen * kEmptyStep; continue; }

        float d = ErodeDetail(base, wp, windWorld) * density;
        if (d > 0.001f)
        {
            float lightT = LightMarch(wp, sunDir, windWorld);
            // Beer-Powder: 太陽側の縁を暗く落として立体感を強調。
            float powder = 1.0f - exp(-d * stepLen * kExtinction * 2.0f);
            powder = lerp(1.0f, powder, kPowder);

            float3 sun = lightCol * lightT * (phase + silver) * powder;
            float3 inScatter = (sun + ambient) * albedo * d;

            // ステップ区間で消散を解析積分してエネルギー保存させる。
            float sigmaE    = max(d * kExtinction, 1e-5f);
            float stepTrans = exp(-sigmaE * stepLen);
            scatter += transmittance * inScatter * (1.0f - stepTrans) / sigmaE;
            transmittance *= stepTrans;
        }
        t += stepLen;
    }

    float alpha = saturate(1.0f - transmittance);
    return float4(scatter, alpha);
}
