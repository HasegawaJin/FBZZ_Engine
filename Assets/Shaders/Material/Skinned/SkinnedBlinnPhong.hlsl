// FBZZ Engine
// Material/Skinned/SkinnedBlinnPhong.hlsl | Material
// GPU スキニング + Blinn-Phong 鏡面反射 + 法線マップ + PCF シャドウ
// PS ロジックは Surface/BlinnPhong.hlsl と完全に一致させること。

#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Platform/DX11.hlsli"
#include "Rendering/Lighting.hlsli"
#include "Rendering/Shadow.hlsli"

Texture2D<float>       texShadow   : register(TEX_SHADOW);
Texture2D              texAlbedo   : register(TEX_ALBEDO);
Texture2D              texNormal   : register(TEX_NORMAL);
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
