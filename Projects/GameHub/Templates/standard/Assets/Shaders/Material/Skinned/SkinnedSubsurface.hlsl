// FBZZ Engine
// Material/Skinned/SkinnedSubsurface.hlsl | Material
// GPU スキニング + 簡易サブサーフェス スキャッタリング
// PS ロジックは Surface/Subsurface.hlsl と完全に一致させること。

#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Platform/DX11.hlsli"
#include "Rendering/Lighting.hlsli"
#include "Rendering/Shadow.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;
    float  roughness;
    float  normalStrength;
    float  occlusionStrength;
    float  alphaCutoff;
    float3 emissiveColor;
    float  emissiveScale;
    float2 uvTiling;
    float2 uvOffset;
    uint   textureMask;
};

Texture2D<float>       texShadow   : register(TEX_SHADOW);
Texture2D              texAlbedo   : register(TEX_ALBEDO);
Texture2D              texNormal   : register(TEX_NORMAL);
Texture2D              texAO       : register(TEX_AO);
SamplerState           sampDefault : register(SAMPLER_DEFAULT);
SamplerComparisonState sampShadow  : register(SAMPLER_SHADOW);

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
    float2 uv = p.uv * uvTiling + uvOffset;

    float4 albedoSample = (textureMask & (1u << 0))
        ? texAlbedo.Sample(sampDefault, uv)
        : albedo;
    float3 col   = albedoSample.rgb * albedo.rgb;
    float  alpha = albedoSample.a  * albedo.a;
    clip(alpha - alphaCutoff);

    float3 N = normalize(p.normal);
    if (textureMask & (1u << 1))
    {
        float3 ns = texNormal.Sample(sampDefault, uv).rgb;
        float3 nm = ApplyNormalMap(ns, N, normalize(p.tangent));
        N = normalize(lerp(N, nm, normalStrength));
    }

    float ao = 1.0f;
    if (textureMask & (1u << 4))
        ao = lerp(1.0f, texAO.Sample(sampDefault, uv).r, occlusionStrength);

    float3 V      = normalize(cameraPos - p.worldPos);
    float3 L      = normalize(-lightDir);
    float  shadow = ComputeShadow(texShadow, sampShadow, p.worldPos,
                                  lightViewProjection, shadowMapTexelSize, shadowBias, N, L);

    float  wrap       = roughness * 0.8f;
    float  NdotL_wrap = saturate((dot(N, L) + wrap) / ((1.0f + wrap) * (1.0f + wrap)));
    float3 ambient    = col * 0.08f * ao;
    float3 diffuse    = col * lightColor * lightIntensity * NdotL_wrap * shadow;

    float  backScatter = pow(saturate(dot(V, -L)), 2.5f) * shadow;
    float3 scatter     = emissiveColor * emissiveScale
                       * (backScatter + 0.15f)
                       * lightColor * lightIntensity * ao;

    float3 result = ambient + diffuse + scatter;

    [loop] for (int pi = 0; pi < pointLightCount; ++pi)
    {
        float3 toLight = pointLights[pi].position - p.worldPos;
        float  dist    = length(toLight);
        float3 Lp      = toLight / dist;
        float  atten   = LightAttenuation(dist, pointLights[pi].range);
        float  ndotlp  = saturate((dot(N, Lp) + wrap) / ((1.0f + wrap) * (1.0f + wrap)));
        result += col * pointLights[pi].color * pointLights[pi].intensity * atten * ndotlp;
    }
    [loop] for (int si = 0; si < spotLightCount; ++si)
    {
        float3 toLight = spotLights[si].position - p.worldPos;
        float  dist    = length(toLight);
        float3 Ls      = toLight / dist;
        float  atten   = LightAttenuation(dist, spotLights[si].range);
        float  cone    = SpotConeWeight(Ls, spotLights[si].direction,
                             spotLights[si].innerCos, spotLights[si].outerCos);
        float  ndotls  = saturate((dot(N, Ls) + wrap) / ((1.0f + wrap) * (1.0f + wrap)));
        result += col * spotLights[si].color * spotLights[si].intensity * atten * cone * ndotls;
    }

    return float4(result, alpha);
}
