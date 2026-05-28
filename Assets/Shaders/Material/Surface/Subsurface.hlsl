// FBZZ Engine
// Material/Surface/Subsurface.hlsl | Material
// 簡易サブサーフェス スキャッタリング (SSS) — 皮膚・蝋・葉など透過感のある素材向け
//
// ラップ拡散 (wrap diffuse) で影境界を柔らかくし、
// ビュー依存バックスキャターで薄い部分の透過光を再現する。
//
// emissiveColor  : サブサーフェス散乱色 (皮膚なら赤みがかった肌色)
// emissiveScale  : 散乱強度 (0 = 通常 Lambert, 1 = 強い SSS)
// roughness      : ラップ量制御 (高いほど影側にも光が回り込む)

#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Platform/DX11.hlsli"
#include "Rendering/Lighting.hlsli"
#include "Rendering/Shadow.hlsli"

Texture2D<float>       texShadow   : register(TEX_SHADOW);
Texture2D              texAlbedo   : register(TEX_ALBEDO);
Texture2D              texNormal   : register(TEX_NORMAL);
Texture2D              texAO       : register(TEX_AO);
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

    // ラップ拡散: roughness が高いほど光が影側に回り込む。
    // wrap=0 で通常 Lambert、wrap=1 で半球全体がほぼ均等に照らされる。
    float  wrap        = roughness * 0.8f;
    float  NdotL_wrap  = saturate((dot(N, L) + wrap) / ((1.0f + wrap) * (1.0f + wrap)));
    float3 ambient     = col * 0.08f * ao;
    float3 diffuse     = col * lightColor * lightIntensity * NdotL_wrap * shadow;

    // ビュー依存バックスキャター: カメラ-ライト-サーフェス が直線に近いほど強い透過光。
    // 薄い素材 (耳、手など) で裏側からの透過光を模倣する。
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
