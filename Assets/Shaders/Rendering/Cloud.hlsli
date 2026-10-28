/// @file    Cloud.hlsli
/// @brief   空に描く雲の値ノイズと FBM。
/// @author  Hasegawa Jin
/// @date    2026-06-23

/// @note Cloud.hlsli | Rendering
/// @note FBM ノイズによる手続き型雲レイヤー (Skydome.hlsl から使用)
#ifndef CLOUD_HLSLI
#define CLOUD_HLSLI

#include "Common/Random.hlsli"

/// @note 2D 値ノイズ — bilinear 補間

/// @note FBM — 4 オクターブ
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

/// @note 雲レイヤーの密度
/// @note rayDir : 視線方向 (正規化)
/// @note cloudH : 雲層の正規化高さ (0=地平線, 1=真上)
/// @note time   : アニメーション用時間 (秒)

float CloudDensity(float3 rayDir, float cloudH, float time)
{
    if (rayDir.y < 0.02f) return 0.0f;

    float  t      = cloudH / rayDir.y;
    float2 uv     = rayDir.xz * (t * 0.35f);
    float2 animUV = uv + float2(time * 0.012f, time * 0.007f);

    float density = CloudFBM(animUV) - 0.40f;
    return saturate(density * 2.8f);
}

/// @note 雲の色と透過率
/// @note 戻り値 : rgb = 雲の照明色, a = 不透明度 [0, 1]

float4 ComputeCloud(float3 rayDir, float3 sunDir,
                    float3 atmosColor, float time, float sunIntensity)
{
    const float CLOUD_H     = 0.22f;
    const float SHADOW_BIAS = 0.10f;

    float density = CloudDensity(rayDir, CLOUD_H, time);
    if (density <= 0.001f) return float4(atmosColor, 0.0f);

    /// @note 太陽方向へオフセットしたサンプルでセルフシャドウを近似
    float3 shadowRay     = normalize(rayDir + sunDir * SHADOW_BIAS);
    float  shadowDensity = CloudDensity(shadowRay, CLOUD_H, time);
    float  shadow        = exp(-shadowDensity * 3.0f);

    /// @note 太陽方向との一致度。前方散乱で太陽側を向いた雲縁が明るく輝く（シルバーライニング）。
    /// @note 従来は upness（真上向き）だけで照明しており、太陽がどこにあっても雲は天頂が最も明るく、
    /// @note 逆光時の縁取りが表現できなかった。sunDir との内積を主光として加え、立体感を出す。
    float  sunAmount    = saturate(dot(rayDir, sunDir));
    float  silverLining = pow(sunAmount, 6.0f);

    /// @note 照明 : 上面 = 日向 / 下面 = 大気光
    float  upness   = saturate(dot(rayDir, float3(0.0f, 1.0f, 0.0f)));
    float3 sunColor = float3(1.0f, 0.95f, 0.85f) * (sunIntensity * 0.06f);
    float3 ambient  = atmosColor * 0.45f;
    float3 cloudLit = sunColor * shadow * (upness + silverLining * 1.5f)
                    + ambient * (1.0f - upness * 0.5f);

    return float4(cloudLit, saturate(density * 0.92f));
}

#endif ///< @note CLOUD_HLSLI
