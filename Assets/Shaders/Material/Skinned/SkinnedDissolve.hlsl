/// @file    SkinnedDissolve.hlsl
/// @brief   Skinned PBR dissolve shading with an emissive transition edge.
/// @author  Hasegawa Jin
/// @date    2026-05-28

#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Rendering/DissolveNoise.hlsli"
#include "Common/Structs.hlsli"
#include "Common/MaterialVertex.hlsli"
#include "Common/Skinning.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/Lighting.hlsli"
#include "Rendering/Shadow.hlsli"
#include "Rendering/SpecularAA.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Common/MaterialTextures.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;
    float  metallic;
    float  roughness;
    float  normalStrength;
    float  occlusionStrength;
    float3 emissiveColor;
    float  emissiveScale;
    float2 uvTiling;
    float2 uvOffset;
    float  alphaCutoff;
    float3 _pad0;
    uint   textureMask;
    float3 _pad1;

    /// @note Material::Upload writes these bindless texture indices; declared slots appear in the Inspector.
    uint texAlbedoIndex;
    uint texNormalIndex;
    uint texMetallicIndex;
    uint texEmissiveIndex;
    uint texAOIndex;
};

FBZZ_TEX2D_T(float, texShadow, TEX_SHADOW_SLOT);
FBZZ_MATERIAL_TEX(texAlbedo, texAlbedoIndex);
FBZZ_MATERIAL_TEX(texNormal, texNormalIndex);
FBZZ_MATERIAL_TEX(texMetallicRough, texMetallicIndex);
FBZZ_MATERIAL_TEX(texEmissive, texEmissiveIndex);
FBZZ_MATERIAL_TEX(texAO, texAOIndex);
SamplerState           sampDefault      : register(SAMPLER_DEFAULT);
SamplerComparisonState sampShadow       : register(SAMPLER_SHADOW);

/// @brief Blend the four bone transforms by the vertex weights.

PSInput VSMain(SkinnedVSInput v)
{
    return TransformSkinnedMaterialVertex(v);
}

/// @brief Hash a 2D lattice coordinate into [0, 1].

/// @brief Evaluate value noise with smooth interpolation between lattice values.

/// @brief Combine four value-noise octaves into the dissolve mask.

float4 PSMain(PSInput p) : SV_Target0
{
    float2 uv = p.uv * uvTiling + uvOffset;

    float4 albedoSample = (textureMask & (1u << 0))
        ? texAlbedo.Sample(sampDefault, uv)
        : albedo;
    float3 col   = albedoSample.rgb * albedo.rgb;
    float  alpha = albedoSample.a  * albedo.a;

    float  mask      = DissolveMask(uv);
    float  edgeWidth = 0.06f;
    clip(mask - alphaCutoff);
    float  edgeFactor = saturate((mask - alphaCutoff) / edgeWidth);

    float3 N = normalize(p.normal);
    if (textureMask & (1u << 1))
    {
        float3 ns = texNormal.Sample(sampDefault, uv).rgb;
        float3 nm = ApplyNormalMap(ns, N, normalize(p.tangent));
        N = normalize(lerp(N, nm, normalStrength));
    }

    float met   = metallic;
    float rough = roughness;
    if (textureMask & (1u << 2))
    {
        float2 mr = texMetallicRough.Sample(sampDefault, uv).gb;
        rough = mr.x;
        met   = mr.y;
    }
    rough = FilterSpecularRoughness(N, saturate(rough));

    float ao = 1.0f;
    if (textureMask & (1u << 4))
        ao = lerp(1.0f, texAO.Sample(sampDefault, uv).r, occlusionStrength);

    float3 V      = normalize(cameraPos - p.worldPos);
    float3 L      = normalize(-lightDir);
    float  shadow = ComputeShadow(texShadow, sampShadow, p.worldPos,
                                  lightViewProjection, shadowMapTexelSize, shadowBias, N, L);
    /// @note Skinned meshes bypass GBuffer, so screen-space AO and contact shadows would sample background pixels.
    FBZZWaterMediumSample receiverMedium;
    const bool receiverInWater = FBZZ_QueryWaterReceiverMedium(p.worldPos, receiverMedium);
    float3 result = Lighting_PBR(p.worldPos, N, V, L, col, met, rough,
                                 lightColor, lightIntensity, shadow, ao);

    /// @note ClusteredLights selects the Point/Spot source from the b3 array, structured buffer, or cluster list.
    FBZZ_PUNCTUAL_BEGIN(p.worldPos, p.svPosition.xy, N)
        const float3 sourceTransmittance = Lighting_WaterPunctualTransmittance(
            p.worldPos, _fbzzLight, ps, receiverInWater, receiverMedium);
        result += Lighting_PBR_Direct(N, V, ps.L, col, met, saturate(rough + ps.roughnessBias),
            ps.color * sourceTransmittance, ps.intensity);
    FBZZ_PUNCTUAL_END

    float3 emissiveTex = (textureMask & (1u << 3))
        ? texEmissive.Sample(sampDefault, uv).rgb
        : float3(1.0f, 1.0f, 1.0f);
    result += emissiveTex * emissiveColor * emissiveScale;
    /// @note The edge band is twice the regular emissive scale; edgeFactor is one outside the band and zero at the cutoff.
    result += emissiveColor * emissiveScale * 2.0f * (1.0f - edgeFactor);

    return float4(result, alpha);
}
