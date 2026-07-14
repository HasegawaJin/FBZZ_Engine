// FBZZ Engine
// Material/Surface/Unlit.hlsl | Material
// ライティングなし — アルベドをそのまま出力する
#ifndef UNLIT_HLSL
#define UNLIT_HLSL

#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Platform/Backend.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;       // RGBA ベースカラー  offset 0
    uint   textureMask;  // テクスチャフラグ   offset 16
};

Texture2D    texAlbedo   : register(TEX_ALBEDO);
SamplerState sampDefault : register(SAMPLER_DEFAULT);

PSInput VSMain(VSInput v)
{
    PSInput o;
    float4 worldPos4 = mul(float4(v.position, 1.0f), world);
    o.worldPos   = worldPos4.xyz;
    o.svPosition = mul(worldPos4, viewProjection);
    o.normal     = normalize(mul(v.normal,  (float3x3)worldInvTranspose));
    o.tangent    = normalize(mul(v.tangent, (float3x3)world));
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

#endif // UNLIT_HLSL
