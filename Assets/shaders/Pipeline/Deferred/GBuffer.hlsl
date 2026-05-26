// FBZZ Engine
// Pipeline/Deferred/GBuffer.hlsl | Pipeline
// ジオメトリパス — 法線マップ・PBR テクスチャを 2 枚の MRT に書き出す
//
// MRT レイアウト:
//   SV_Target0 (RGBA16F): RGB=albedo,      A=roughness
//   SV_Target1 (RGBA16F): RGB=worldNormal, A=metallic

#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Common/Space.hlsli"
#include "Platform/DX11.hlsli"

Texture2D    texAlbedo        : register(TEX_ALBEDO);
Texture2D    texNormal        : register(TEX_NORMAL);
Texture2D    texMetallicRough : register(TEX_METALLIC_ROUGH);
SamplerState sampDefault      : register(SAMPLER_DEFAULT);

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

GBufferOut PSMain(PSInput p)
{
    // Albedo
    float3 col = (textureMask & (1u << 0))
        ? texAlbedo.Sample(sampDefault, p.uv).rgb
        : albedo;

    // Normal (法線マップがあれば TBN で変換)
    float3 N = normalize(p.normal);
    if (textureMask & (1u << 1))
    {
        float3 ns = texNormal.Sample(sampDefault, p.uv).rgb;
        N = ApplyNormalMap(ns, N, normalize(p.tangent));
    }

    // Metallic / Roughness (glTF 規約: G=roughness, B=metallic)
    float met   = metallic;
    float rough = roughness;
    if (textureMask & (1u << 2))
    {
        float2 mr = texMetallicRough.Sample(sampDefault, p.uv).gb;
        rough = mr.x;
        met   = mr.y;
    }

    GBufferOut o;
    o.albedoRoughness = float4(col, rough);
    // 法線は [-1,1] → [0,1] にエンコード (復元: n*2-1)
    o.normalMetallic  = float4(N * 0.5f + 0.5f, met);
    return o;
}
