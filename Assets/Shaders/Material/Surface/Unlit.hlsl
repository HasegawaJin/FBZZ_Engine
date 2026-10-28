/// @file    Unlit.hlsl
/// @brief   頂点 RGBA とテクスチャを乗算し、Sprite の明示要求時だけ既存 Lambert 照明を評価する。
/// @author  Hasegawa Jin
/// @date    2026-06-23
#ifndef UNLIT_HLSL
#define UNLIT_HLSL

#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Common/Color.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/Lighting.hlsli"
#include "Rendering/Shadow.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Common/MaterialTextures.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;
    uint   textureMask;
    uint texAlbedoIndex;
    /// @note 既定 0 は従来の Unlit。Presentation defaults が 1 を渡した Sprite だけ光を受ける。
    uint receiveLighting;
};

FBZZ_MATERIAL_TEX(texAlbedo, texAlbedoIndex);
FBZZ_TEX2D_T(float, texShadow, TEX_SHADOW_SLOT);
SamplerState sampDefault : register(SAMPLER_DEFAULT);
SamplerComparisonState sampShadow : register(SAMPLER_SHADOW);

struct UnlitPSInput
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
    float3 worldPos   : TEXCOORD1;
    float3 normal     : TEXCOORD2;
    float4 color      : COLOR;
};

/// @note Vertex の末尾 COLOR を宣言し、Line の両端 alpha まで補間へ渡す。色なしの標準 Vertex は既定白。
UnlitPSInput VSMain(VSInputColor v)
{
    UnlitPSInput o;
    float4 worldPos4 = mul(float4(v.position, 1.0f), world);
    o.svPosition = mul(worldPos4, viewProjection);
    o.uv         = v.uv;
    o.worldPos   = worldPos4.xyz;
    o.normal     = normalize(mul(v.normal, (float3x3)worldInvTranspose));
    o.color      = v.color;
    return o;
}

/// @note RGB / alpha とも頂点 × 材質 × テクスチャ。照明は RGB だけに適用し、合成方法は .mat に委ねる。
float4 PSMain(UnlitPSInput p, bool frontFace : SV_IsFrontFace) : SV_Target0
{
    float4 color = albedo * p.color;
    if (textureMask & 1u) {
        float4 textureColor = texAlbedo.Sample(sampDefault, p.uv);
        /// @note UNORM の colour albedo は既存 PBR と同じ sRGB 入力。照明へ渡す RGB だけを線形へ戻し、alpha は保持する。
        if (receiveLighting != 0u) textureColor.rgb = SRGBToLinear(textureColor.rgb);
        color *= textureColor;
    }
    if (receiveLighting != 0u) {
        const float3 N = frontFace ? normalize(p.normal) : -normalize(p.normal);
        const float3 L = normalize(-lightDir);
        float shadow = ComputeShadow(texShadow, sampShadow, p.worldPos,
            lightViewProjection, shadowMapTexelSize, shadowBias, N, L);
        shadow *= FBZZ_ScreenContactShadow(p.svPosition.xy);
        float3 lit = Lighting_Lambert(N, L, color.rgb, lightColor, lightIntensity, shadow);
        FBZZ_PUNCTUAL_BEGIN(p.worldPos, p.svPosition.xy, N)
            lit += Lighting_Lambert_Direct(N, ps.L, color.rgb, ps.color, ps.intensity);
        FBZZ_PUNCTUAL_END
        color.rgb = lit;
    }
    return color;
}

#endif /// @note UNLIT_HLSL
