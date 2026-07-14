// FBZZ Engine
// Material/Surface/BlinnPhong.hlsl | Material
// Blinn-Phong 鏡面反射 + 法線マップ + PCF シャドウ

#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/Lighting.hlsli"
#include "Rendering/Shadow.hlsli"
// ApplyNormalMap は Shadow.hlsli → Space.hlsli 経由で提供される

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;       // RGBA ベースカラー         offset 0
    float  roughness;    // スペキュラの広がり [0,1]   offset 16
    uint   textureMask;  // テクスチャフラグ           offset 20
};

Texture2D<float>       texShadow   : register(TEX_SHADOW);
Texture2D              texAlbedo   : register(TEX_ALBEDO);
Texture2D              texNormal   : register(TEX_NORMAL);
SamplerState           sampDefault : register(SAMPLER_DEFAULT);
SamplerComparisonState sampShadow  : register(SAMPLER_SHADOW);

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
    float3 col = (textureMask & 1u)
        ? texAlbedo.Sample(sampDefault, p.uv).rgb
        : albedo.rgb;

    float3 N = normalize(p.normal);
    if (textureMask & (1u << 1))
    {
        float3 normalSample = texNormal.Sample(sampDefault, p.uv).rgb;
        N = ApplyNormalMap(normalSample, N, normalize(p.tangent));
    }

    float3 V      = normalize(cameraPos - p.worldPos);
    float3 L      = normalize(-lightDir);
    float  shadow = ComputeShadow(texShadow, sampShadow, p.worldPos,
                                  lightViewProjection, shadowMapTexelSize, shadowBias, N, L);
    float3 result = Lighting_BlinnPhong(N, V, L, col, roughness,
                                        lightColor, lightIntensity, shadow);

    [loop] for (int pi = 0; pi < pointLightCount; ++pi)
    {
        float3 toLight = pointLights[pi].position - p.worldPos;
        float  dist    = length(toLight);
        float3 Lp      = toLight / dist;
        float  atten   = LightAttenuation(dist, pointLights[pi].range);
        result += Lighting_BlinnPhong_Direct(N, V, Lp, col, roughness,
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
        result += Lighting_BlinnPhong_Direct(N, V, Ls, col, roughness,
                      spotLights[si].color, spotLights[si].intensity * atten * cone);
    }
    return float4(result, 1.0f);
}
