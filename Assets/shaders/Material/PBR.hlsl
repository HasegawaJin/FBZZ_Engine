// FBZZ Engine
// PBR.hlsl | Material
// Cook-Torrance PBR フォワードパス (法線マップ / AO / エミッシブ / PCF シャドウ)

#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Platform/DX11.hlsli"
#include "Rendering/Lighting.hlsli"
#include "Rendering/Shadow.hlsli"
// ApplyNormalMap は Shadow.hlsli → Space.hlsli 経由で提供される

Texture2D<float>       texShadow        : register(TEX_SHADOW);
Texture2D              texAlbedo        : register(TEX_ALBEDO);
Texture2D              texNormal        : register(TEX_NORMAL);
Texture2D              texMetallicRough : register(TEX_METALLIC_ROUGH);
Texture2D              texEmissive      : register(TEX_EMISSIVE);
Texture2D              texAO            : register(TEX_AO);
SamplerState           sampDefault      : register(SAMPLER_DEFAULT);
SamplerComparisonState sampShadow       : register(SAMPLER_SHADOW);

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
    // Albedo
    float3 col = (textureMask & (1u << 0))
        ? texAlbedo.Sample(sampDefault, p.uv).rgb
        : albedo;

    // Normal
    float3 N = normalize(p.normal);
    if (textureMask & (1u << 1))
    {
        float3 ns = texNormal.Sample(sampDefault, p.uv).rgb;
        N = ApplyNormalMap(ns, N, normalize(p.tangent));
    }

    // Metallic / Roughness (glTF 規約: G チャンネル = roughness, B チャンネル = metallic)
    float met   = metallic;
    float rough = roughness;
    if (textureMask & (1u << 2))
    {
        float2 mr = texMetallicRough.Sample(sampDefault, p.uv).gb;
        rough = mr.x;
        met   = mr.y;
    }

    // AO
    float ao = 1.0f;
    if (textureMask & (1u << 4))
        ao = texAO.Sample(sampDefault, p.uv).r;

    float3 V      = normalize(cameraPos - p.worldPos);
    float3 L      = normalize(-lightDir);
    float  shadow = ComputeShadow(texShadow, sampShadow, p.worldPos,
                                  lightViewProjection, shadowMapTexelSize, shadowBias, N, L);
    float3 result = Lighting_PBR(N, V, L, col, met, rough,
                                 lightColor, lightIntensity, shadow, ao);

    [loop] for (int pi = 0; pi < pointLightCount; ++pi)
    {
        float3 toLight = pointLights[pi].position - p.worldPos;
        float  dist    = length(toLight);
        float3 Lp      = toLight / dist;
        float  atten   = LightAttenuation(dist, pointLights[pi].range);
        result += Lighting_PBR_Direct(N, V, Lp, col, met, rough,
                      pointLights[pi].color, pointLights[pi].intensity * atten);
    }
    [loop] for (int si = 0; si < spotLightCount; ++si)
    {
        float3 toLight = spotLights[si].position - p.worldPos;
        float  dist    = length(toLight);
        float3 Ls      = toLight / dist;
        float  atten   = LightAttenuation(dist, spotLights[si].range);
        float  cone    = SpotConeWeight(Ls, spotLights[si].direction,
                             spotLights[si].innerCos, spotLights[si].outerCos);
        result += Lighting_PBR_Direct(N, V, Ls, col, met, rough,
                      spotLights[si].color, spotLights[si].intensity * atten * cone);
    }

    // Emissive
    if (textureMask & (1u << 3))
        result += texEmissive.Sample(sampDefault, p.uv).rgb * emissiveScale;

    return float4(result, 1.0f);
}