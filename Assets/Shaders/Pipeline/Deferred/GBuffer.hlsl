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
#include "Common/Color.hlsli"
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
    float2 uv = p.uv * uvTiling + uvOffset;

    // Albedo + tint
    // sRGB テクスチャを線形空間にデコードしてから tint (線形) を乗算する。
    // テクスチャなし時は (1,1,1) として albedo.rgb をそのまま使用 (SRGBToLinear(1)=1)。
    float4 rawAlbedo = (textureMask & (1u << 0))
        ? texAlbedo.Sample(sampDefault, uv)
        : float4(1.0f, 1.0f, 1.0f, 1.0f);
    float3 col  = SRGBToLinear(rawAlbedo.rgb) * albedo.rgb;
    float  alpha = rawAlbedo.a * albedo.a;
    clip(alpha - alphaCutoff);

    // Normal (法線マップがあれば TBN で変換、normalStrength でブレンド)
    float3 N = normalize(p.normal);
    if (textureMask & (1u << 1))
    {
        float3 ns = texNormal.Sample(sampDefault, uv).rgb;
        float3 nm = ApplyNormalMap(ns, N, normalize(p.tangent));
        N = normalize(lerp(N, nm, normalStrength));
    }

    // Metallic / Roughness (glTF 規約: G=roughness, B=metallic)
    float met   = metallic;
    float rough = roughness;
    if (textureMask & (1u << 2))
    {
        float2 mr = texMetallicRough.Sample(sampDefault, uv).gb;
        rough = mr.x;
        met   = mr.y;
    }

    GBufferOut o;
    o.albedoRoughness = float4(col, rough);
    // 法線は [-1,1] → [0,1] にエンコード (復元: n*2-1)
    o.normalMetallic  = float4(N * 0.5f + 0.5f, met);
    return o;
}
