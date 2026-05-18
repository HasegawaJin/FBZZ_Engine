// FBZZ Engine
// Shadow.hlsli | Rendering
// PCF シャドウサンプリング
#ifndef SHADOW_HLSLI
#define SHADOW_HLSLI

#include "Common/Space.hlsli"

// =========================================================================
// PCF (Percentage Closer Filtering) シャドウ
//   shadowMap    : Texture2D<float> — シャドウデプスバッファ
//   shadowSampler: SamplerComparisonState (D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR)
//   uv           : シャドウマップ UV [0,1]
//   depth        : ライト空間の深度値 (比較基準)
//   texelSize    : 1.0 / シャドウマップ解像度
//   radius       : PCF カーネル半径 (1 = 3x3, 2 = 5x5)
//   戻り値        : 0.0=完全に影, 1.0=完全に照らされている
// =========================================================================
float SampleShadowPCF(Texture2D<float> shadowMap,
                      SamplerComparisonState shadowSampler,
                      float2 uv, float depth, float2 texelSize, int radius)
{
    float shadow = 0.0f;
    float total  = 0.0f;

    for (int y = -radius; y <= radius; ++y)
    for (int x = -radius; x <= radius; ++x)
    {
        float2 offset = float2(x, y) * texelSize;
        shadow += shadowMap.SampleCmpLevelZero(shadowSampler, uv + offset, depth);
        total  += 1.0f;
    }

    return shadow / total;
}

// =========================================================================
// ComputeShadow — ワールド座標からシャドウ係数を計算する
//   bias で Self-Shadow アクネを防ぐ。
//   UV が [0,1] 外 (ライト錐台外) は常に 1.0 (照らされている) を返す。
// =========================================================================
float ComputeShadow(Texture2D<float> shadowMap,
                    SamplerComparisonState shadowSampler,
                    float3 worldPos, float4x4 lightVP,
                    float2 texelSize, float bias)
{
    float2 uv;
    float  depth;
    WorldToShadowUV(worldPos, lightVP, uv, depth);

    // ライト錐台の外は影なし
    if (any(uv < 0.0f) || any(uv > 1.0f))
        return 1.0f;

    return SampleShadowPCF(shadowMap, shadowSampler, uv, depth - bias, texelSize, 1);
}

#endif // SHADOW_HLSLI