/// @file    SkinnedUnlit.hlsl
/// @brief   GPU skinning と tint・texture の RGBA 乗算、明示要求時の既存 Lambert 照明。
/// @author  Hasegawa Jin
/// @date    2026-06-23

#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Common/Skinning.hlsli"
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
    /// @note Surface/Unlit と同じ明示 option。既定 0 では lighting のみ省略し、tint と alpha は保持する。
    uint receiveLighting;
};

FBZZ_MATERIAL_TEX(texAlbedo, texAlbedoIndex);
FBZZ_TEX2D_T(float, texShadow, TEX_SHADOW_SLOT);
SamplerState sampDefault : register(SAMPLER_DEFAULT);
SamplerComparisonState sampShadow : register(SAMPLER_SHADOW);

struct SkinnedUnlitPSInput
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
    float3 worldPos   : TEXCOORD1;
    float3 normal     : TEXCOORD2;
};



SkinnedUnlitPSInput VSMain(SkinnedVSInput v)
{
    SkinnedUnlitPSInput o;
    float4x4 skin    = BlendSkinMatrix(v);
    float4 localPos  = mul(float4(v.position, 1.0f), skin);
    float4 worldPos4 = mul(localPos, world);
    o.svPosition = mul(worldPos4, viewProjection);
    o.uv         = v.uv;
    o.worldPos   = worldPos4.xyz;
    float3 localNormal = mul(v.normal, (float3x3)skin);
    o.normal     = normalize(mul(localNormal, (float3x3)worldInvTranspose));
    return o;
}

/// @note SkinnedVertex は COLOR を持たないため頂点 tint は白。Surface/Unlit と同じ材質 × texture RGBA を返す。
float4 PSMain(SkinnedUnlitPSInput p, bool frontFace : SV_IsFrontFace) : SV_Target0
{
    float4 color = albedo;
    if (textureMask & 1u) {
        float4 textureColor = texAlbedo.Sample(sampDefault, p.uv);
        /// @note Colour albedo は標準 PBR と同じ sRGB 入力。lighting の RGB だけを線形へ戻し、alpha は保持する。
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
