// FBZZ Engine
// Material/Skinned/FallbackSkinned.hlsl | Material
// shader 未設定または Surface 誤適用時の skinned mesh 用原色紫 Unlit フォールバック
#ifndef FALLBACK_SKINNED_HLSL
#define FALLBACK_SKINNED_HLSL

#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Platform/Backend.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;
    uint   textureMask;
};

Texture2D    texAlbedo   : register(TEX_ALBEDO);
SamplerState sampDefault : register(SAMPLER_DEFAULT);

float4x4 BlendSkinMatrix(SkinnedVSInput v)
{
    return boneMatrices[v.boneIndices.x] * v.boneWeights.x
         + boneMatrices[v.boneIndices.y] * v.boneWeights.y
         + boneMatrices[v.boneIndices.z] * v.boneWeights.z
         + boneMatrices[v.boneIndices.w] * v.boneWeights.w;
}

PSInput VSMain(SkinnedVSInput v)
{
    PSInput o;
    float4x4 skin    = BlendSkinMatrix(v);
    float4 localPos  = mul(float4(v.position, 1.0f), skin);
    float3 localN    = normalize(mul(v.normal,  (float3x3)skin));
    float3 localT    = normalize(mul(v.tangent, (float3x3)skin));
    float4 worldPos4 = mul(localPos, world);
    o.worldPos   = worldPos4.xyz;
    o.svPosition = mul(worldPos4, viewProjection);
    o.normal     = normalize(mul(localN, (float3x3)worldInvTranspose));
    o.tangent    = normalize(mul(localT, (float3x3)world));
    o.uv         = v.uv;
    return o;
}

float4 PSMain(PSInput p) : SV_Target0
{
    float3 color = (textureMask & 1u)
        ? texAlbedo.Sample(sampDefault, p.uv).rgb
        : albedo.rgb;
    return float4(color, 1.0f);
}

#endif // FALLBACK_SKINNED_HLSL
