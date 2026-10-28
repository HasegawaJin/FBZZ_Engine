/// @file    MaterialChannel.hlsl
/// @brief   MaterialChannel.hlsl の既存描画と共通頂点契約。
/// @author  Hasegawa Jin
/// @date    2026-09-16

#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Common/MaterialVertex.hlsli"
#include "Platform/Backend.hlsli"
#include "Common/BindlessIndices.hlsli"

/// @note cbuffer 名を MaterialConstants にしないか:
/// @note シェーダーリフレクションは "MaterialConstants" という名前を «.mat が編集できる
/// @note パラメータ» として拾う (DX11Shader/DX12Shader::BuildDescriptor)。このシェーダーは
/// @note プレビュー専用で .mat から選ばれてはならないので、別名にして拾わせない。
cbuffer MaterialChannelConstants : register(CB_MATERIAL)
{
    float4 channelBaseColor;   /// @note rgb = albedo フォールバック, a = アルファ
    float4 channelPbr;         /// @note x=metallic y=roughness z=occlusionStrength w=normalStrength
    float4 channelUv;          /// @note xy=uvTiling zw=uvOffset
    float4 channelEmissive;    /// @note rgb=emissiveColor a=emissiveScale
    uint   channelTextureMask; /// @note bit0..4 = albedo / normal / metallicRough / emissive / ao
    uint   channelMode;
    float2 _channelPad;
};

FBZZ_TEX2D(texAlbedo, TEX_ALBEDO_SLOT);
FBZZ_TEX2D(texNormal, TEX_NORMAL_SLOT);
FBZZ_TEX2D(texMetallicRough, TEX_METALLIC_ROUGH_SLOT);
FBZZ_TEX2D(texEmissive, TEX_EMISSIVE_SLOT);
FBZZ_TEX2D(texAO, TEX_AO_SLOT);
SamplerState sampDefault      : register(SAMPLER_DEFAULT);

#define FBZZ_CHANNEL_ALBEDO    0u
#define FBZZ_CHANNEL_NORMAL    1u
#define FBZZ_CHANNEL_ROUGHNESS 2u
#define FBZZ_CHANNEL_METALLIC  3u
#define FBZZ_CHANNEL_AO        4u
#define FBZZ_CHANNEL_EMISSIVE  5u
#define FBZZ_CHANNEL_UV        6u

PSInput VSMain(VSInput v)
{
    return TransformMaterialVertex(v);
}

float3 ShadingNormal(PSInput p, float2 uv)
{
    float3 N = normalize(p.normal);
    if ((channelTextureMask & 2u) == 0u) return N;

    float3 T = normalize(p.tangent - N * dot(N, p.tangent));
    float3 B = cross(N, T);
    float3 tangentNormal = texNormal.Sample(sampDefault, uv).xyz * 2.0f - 1.0f;
    tangentNormal.xy *= channelPbr.w;
    return normalize(mul(tangentNormal, float3x3(T, B, N)));
}

/// @note UV の向きと密度を同時に読ませる市松。輝度だけの市松は 90 度回転や
/// @note V 反転を見分けられないので、色で U / V の軸を示す。
float3 UvChecker(float2 uv)
{
    float2 cell = floor(uv * 8.0f);
    float  odd  = frac((cell.x + cell.y) * 0.5f) * 2.0f;
    float  tone = lerp(0.22f, 0.82f, odd);
    return float3(tone * frac(uv.x), tone * frac(uv.y), tone * 0.35f);
}

float4 PSMain(PSInput p) : SV_Target0
{
    float2 uv = p.uv * channelUv.xy + channelUv.zw;

    if (channelMode == FBZZ_CHANNEL_UV)
        return float4(UvChecker(uv), 1.0f);

    if (channelMode == FBZZ_CHANNEL_NORMAL)
        return float4(ShadingNormal(p, uv) * 0.5f + 0.5f, 1.0f);

    if (channelMode == FBZZ_CHANNEL_ALBEDO) {
        float3 albedoColor = (channelTextureMask & 1u)
            ? texAlbedo.Sample(sampDefault, uv).rgb
            : channelBaseColor.rgb;
        return float4(albedoColor, 1.0f);
    }

    if (channelMode == FBZZ_CHANNEL_EMISSIVE) {
        float3 emissiveColor = channelEmissive.rgb * channelEmissive.a;
        if (channelTextureMask & 8u)
            emissiveColor *= texEmissive.Sample(sampDefault, uv).rgb;
        return float4(emissiveColor, 1.0f);
    }

    float value = 0.0f;
    if (channelMode == FBZZ_CHANNEL_ROUGHNESS) {
        value = channelPbr.y;
        if (channelTextureMask & 4u) value *= texMetallicRough.Sample(sampDefault, uv).g;
    } else if (channelMode == FBZZ_CHANNEL_METALLIC) {
        value = channelPbr.x;
        if (channelTextureMask & 4u) value *= texMetallicRough.Sample(sampDefault, uv).r;
    } else { /// @note FBZZ_CHANNEL_AO
        float occlusion = (channelTextureMask & 16u)
            ? texAO.Sample(sampDefault, uv).r
            : 1.0f;
        value = lerp(1.0f, occlusion, saturate(channelPbr.z));
    }
    return float4(value.xxx, 1.0f);
}
