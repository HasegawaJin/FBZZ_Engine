/// @file    BakedDepthRestore.hlsl
/// @brief   保存済み静的深度を方向ライトの最遠タイルへ復元する。
/// @author  Hasegawa Jin
/// @date    2026-11-07
#include "Common/Fullscreen.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D(g_packedBakeDepth, 0);

cbuffer BakedDepthRestoreConstants : register(CB_OBJECT)
{
    float4x4 cascadeToBakeClip;
    float bakeDepthScale;
    float maxSmoothDepthStep;
    float sourceDepthBias;
    float bakeRestorePadding;
};

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

float LoadBakeDepth(int2 pixel)
{
    const uint4 bytes = uint4(round(g_packedBakeDepth.Load(int3(pixel, 0)) * 255.0f));
    return asfloat(bytes.x | (bytes.y << 8) | (bytes.z << 16) | (bytes.w << 24));
}

float PSMain(FBZZFullscreenVertex input) : SV_Depth
{
    /// @note 現在の最遠タイルを固定ベイクへ写す。両投影で平行な光線を使うため XY は深度から独立する。
    const float2 cascadeClip = input.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
    const float3 bakeClipAtNear = mul(float4(cascadeClip, 0.0f, 1.0f), cascadeToBakeClip).xyz;
    const float2 bakeUV = bakeClipAtNear.xy * float2(0.5f, -0.5f) + 0.5f;
    uint width, height;
    g_packedBakeDepth.GetDimensions(width, height);
    const float2 texel = saturate(bakeUV) * float2(width, height);
    const int2 maxPixel = int2(width, height) - 1;
    float bakedDepth = LoadBakeDepth(min(int2(texel), maxPixel));
    if (bakedDepth >= 1.0f) return 1.0f;

    if (maxSmoothDepthStep > 0.0f) {
        const float2 samplePosition = texel - 0.5f;
        const int2 lower = int2(floor(samplePosition));
        const int2 pixel0 = clamp(lower, int2(0, 0), maxPixel);
        const int2 pixel1 = clamp(lower + 1, int2(0, 0), maxPixel);
        const float d00 = LoadBakeDepth(pixel0);
        const float d10 = LoadBakeDepth(int2(pixel1.x, pixel0.y));
        const float d01 = LoadBakeDepth(int2(pixel0.x, pixel1.y));
        const float d11 = LoadBakeDepth(pixel1);
        const float nearest = min(min(d00, d10), min(d01, d11));
        const float farthest = max(max(d00, d10), max(d01, d11));
        /// @note 空画素や深度の不連続を跨ぐ補間は、存在しない遮蔽物を作る。
        if (farthest < 1.0f && farthest - nearest <= maxSmoothDepthStep) {
            const float2 weight = frac(samplePosition);
            bakedDepth = lerp(lerp(d00, d10, weight.x), lerp(d01, d11, weight.x), weight.y);
        }
    }
    const float cascadeDepth = (bakedDepth + sourceDepthBias - bakeClipAtNear.z) / bakeDepthScale;
    return cascadeDepth >= 0.0f ? min(cascadeDepth, 1.0f) : 1.0f;
}
