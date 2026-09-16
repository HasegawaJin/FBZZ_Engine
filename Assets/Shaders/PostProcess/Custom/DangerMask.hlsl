/// @file    DangerMask.hlsl
/// @brief   白いマスクを反復し、危機の侵入・鼓動・にじみを合成する。
/// @author  Hasegawa Jin
/// @date    2026-09-15

#include "Common/Constants.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Common/Color.hlsli"
#include "Platform/Backend.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Common/MaterialTextures.hlsli"

FBZZ_TEX2D(texInput, TEX_GBUFFER0_SLOT);
SamplerState sampLinear : register(SAMPLER_LINEAR_CLAMP);

cbuffer MaterialConstants : register(b2)
{
    float tilePixels;
    float maskSoftness;
    float bleedGain;
    float edgeDarken;

    // bindless のテクスチャ添字。Material::Upload が毎フレーム書き込む。
    uint texAlbedoIndex;
};

// WHY cbuffer の後ろへ置くか: 添字フィールドを参照して初期化するため、
//     宣言はフィールドより後ろでなければならない。
FBZZ_MATERIAL_TEX(texMask, texAlbedoIndex);

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

float Mask(float2 uv, float threshold)
{
    const float value = texMask.SampleLevel(sampLinear, frac(uv), 0).a;
    const float softness = max(maskSoftness, 0.02f);
    return smoothstep(threshold - softness, threshold + softness, value);
}

float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    const float3 original = texInput.SampleLevel(sampLinear, p.uv, 0).rgb;
    const float strength = saturate(customIntensity);
    const float pulse = saturate(customParameters2.w);
    const float reach = clamp(customParameters.x, 0.02f, 0.3f)
        * (0.65f + 0.35f * strength + 0.16f * pulse);
    const float edgeDistance = min(min(p.uv.x, 1.0f - p.uv.x), min(p.uv.y, 1.0f - p.uv.y));
    // 中央の視界は完全に素通しし、にじみも周辺に閉じる。
    const float edge = 1.0f - smoothstep(0.0f, reach, edgeDistance);
    const float tile = max(tilePixels, 12.0f) * max(screenSize.y / 1080.0f, 0.25f);
    const float2 uv = p.uv * screenSize.xy / tile
        + float2(0.012f, -0.018f) * customParameters.w;
    const float threshold = clamp(customParameters.y - pulse * 0.14f, 0.1f, 0.9f);
    const float blur = max(customParameters.z, 0.0f) * (1.0f + pulse) / tile;
    const float sharp = Mask(uv, threshold);
    const float soft = (sharp * 2.0f
        + Mask(uv + float2(blur, 0.0f), threshold)
        + Mask(uv - float2(blur, 0.0f), threshold)
        + Mask(uv + float2(0.0f, blur), threshold)
        + Mask(uv - float2(0.0f, blur), threshold)) / 6.0f;
    const float envelope = strength * (0.48f + 0.52f * pulse);
    const float ink = lerp(sharp, soft, saturate(blur * 4.0f)) * edge * envelope;
    const float halo = soft * edge * edge * envelope * max(bleedGain, 0.0f);
    float3 result = original * (1.0f - edge * strength * saturate(edgeDarken));
    result = lerp(result, customParameters2.rgb, saturate(ink * 0.52f));
    result += customParameters2.rgb * halo;
    return float4(lerp(original, result, saturate(customBlend)), 1.0f);
}