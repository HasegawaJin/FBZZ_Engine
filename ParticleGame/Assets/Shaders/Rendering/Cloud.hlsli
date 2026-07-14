// FBZZ Engine
// Cloud.hlsli | Rendering
// FBM ノイズによる手続き型雲レイヤー (Skydome.hlsl から使用)
#ifndef CLOUD_HLSLI
#define CLOUD_HLSLI

#include "Common/Random.hlsli"

// =========================================================================
// 2D 値ノイズ — bilinear 補間
// =========================================================================
float ValueNoise2D(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0f - 2.0f * f);

    float a = Hash2D(i);
    float b = Hash2D(i + float2(1.0f, 0.0f));
    float c = Hash2D(i + float2(0.0f, 1.0f));
    float d = Hash2D(i + float2(1.0f, 1.0f));

    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

// FBM — 4 オクターブ
float CloudFBM(float2 p)
{
    float  v   = 0.0f;
    float  amp = 0.5f;
    float2 q   = p;
    [unroll] for (int i = 0; i < 4; ++i)
    {
        v   += amp * ValueNoise2D(q);
        q   *= 2.1f;
        amp *= 0.48f;
    }
    return v;
}

// =========================================================================
// 雲レイヤーの密度
//   rayDir : 視線方向 (正規化)
//   cloudH : 雲層の正規化高さ (0=地平線, 1=真上)
//   time   : アニメーション用時間 (秒)
// =========================================================================
float CloudDensity(float3 rayDir, float cloudH, float time)
{
    if (rayDir.y < 0.02f) return 0.0f;

    float  t      = cloudH / rayDir.y;
    float2 uv     = rayDir.xz * (t * 0.35f);
    float2 animUV = uv + float2(time * 0.012f, time * 0.007f);

    float density = CloudFBM(animUV) - 0.40f;
    return saturate(density * 2.8f);
}

// =========================================================================
// 雲の色と透過率
//   戻り値 : rgb = 雲の照明色, a = 不透明度 [0, 1]
// =========================================================================
float4 ComputeCloud(float3 rayDir, float3 sunDir,
                    float3 atmosColor, float time, float sunIntensity)
{
    const float CLOUD_H     = 0.22f;
    const float SHADOW_BIAS = 0.10f;

    float density = CloudDensity(rayDir, CLOUD_H, time);
    if (density <= 0.001f) return float4(atmosColor, 0.0f);

    // 太陽方向へオフセットしたサンプルでセルフシャドウを近似
    float3 shadowRay     = normalize(rayDir + sunDir * SHADOW_BIAS);
    float  shadowDensity = CloudDensity(shadowRay, CLOUD_H, time);
    float  shadow        = exp(-shadowDensity * 3.0f);

    // 照明 : 上面 = 日向 / 下面 = 大気光
    float  upness   = saturate(dot(rayDir, float3(0.0f, 1.0f, 0.0f)));
    float3 sunColor = float3(1.0f, 0.95f, 0.85f) * (sunIntensity * 0.06f);
    float3 ambient  = atmosColor * 0.45f;
    float3 cloudLit = sunColor * shadow * upness
                    + ambient * (1.0f - upness * 0.5f);

    return float4(cloudLit, saturate(density * 0.92f));
}

#endif // CLOUD_HLSLI
