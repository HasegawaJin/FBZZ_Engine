// FBZZ Engine
// Material/Surface/Anisotropic.hlsl | Material
// 異方性スペキュラ (Kajiya-Kay モデル) — 髪・ブラッシュドメタル向け
//
// タンジェント方向にスペキュラローブが伸びた金属感・髪質感を再現する。
// UV の Y 方向をファイバー方向 (髪の流れ、研磨方向) に合わせると効果的。
//
// normalStrength : タンジェントを法線方向にシフトする量 (ハイライト帯の位置調整)
// roughness      : スペキュラの広がり (低=細い鋭いハイライト、高=広いぼんやり)
// metallic       : 色付きスペキュラの強度 (1=アルベドで染まる金属ハイライト)

#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Platform/DX11.hlsli"
#include "Rendering/Lighting.hlsli"
#include "Rendering/Shadow.hlsli"

Texture2D<float>       texShadow        : register(TEX_SHADOW);
Texture2D              texAlbedo        : register(TEX_ALBEDO);
Texture2D              texNormal        : register(TEX_NORMAL);
Texture2D              texMetallicRough : register(TEX_METALLIC_ROUGH);
Texture2D              texEmissive      : register(TEX_EMISSIVE);
Texture2D              texAO            : register(TEX_AO);
SamplerState           sampDefault      : register(SAMPLER_DEFAULT);
SamplerComparisonState sampShadow       : register(SAMPLER_SHADOW);

// Kajiya-Kay 異方性スペキュラ項。
// T  : ファイバー方向 (法線シフト済みタンジェント)
// H  : ハーフベクトル
// shininess : 集中度
float KajiyaKaySpec(float3 T, float3 H, float shininess)
{
    float TdotH = dot(T, H);
    float sinTH = sqrt(max(0.0f, 1.0f - TdotH * TdotH));
    return pow(sinTH, shininess);
}

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

    float met   = metallic;
    float rough = roughness;
    if (textureMask & (1u << 2))
    {
        float2 mr = texMetallicRough.Sample(sampDefault, uv).gb;
        rough = mr.x;
        met   = mr.y;
    }

    float ao = 1.0f;
    if (textureMask & (1u << 4))
        ao = lerp(1.0f, texAO.Sample(sampDefault, uv).r, occlusionStrength);

    // normalStrength でタンジェントを法線方向へシフト → ハイライト帯の縦位置を制御。
    float3 T = normalize(p.tangent + N * (normalStrength * 0.3f - 0.15f));

    float3 V      = normalize(cameraPos - p.worldPos);
    float3 L      = normalize(-lightDir);
    float3 H      = normalize(V + L);
    float  shadow = ComputeShadow(texShadow, sampShadow, p.worldPos,
                                  lightViewProjection, shadowMapTexelSize, shadowBias, N, L);

    float  NdotL      = saturate(dot(N, L));
    float  shininess  = max(lerp(512.0f, 2.0f, rough), 2.0f);
    float  specTerm   = KajiyaKaySpec(T, H, shininess);
    // 金属度で色付きスペキュラ / 無彩色スペキュラを補間する。
    float3 specColor  = lerp(float3(1.0f, 1.0f, 1.0f), col, met);
    float3 specular   = specColor * lightColor * lightIntensity
                      * specTerm * (1.0f - rough) * shadow;

    float3 ambient    = col * 0.08f * ao;
    float3 diffuse    = col * lightColor * lightIntensity * NdotL * shadow;
    float3 result     = ambient + diffuse + specular;

    [loop] for (int pi = 0; pi < pointLightCount; ++pi)
    {
        float3 toLight = pointLights[pi].position - p.worldPos;
        float  dist    = length(toLight);
        float3 Lp      = toLight / dist;
        float3 Hp      = normalize(V + Lp);
        float  atten   = LightAttenuation(dist, pointLights[pi].range);
        float  NdotLp  = saturate(dot(N, Lp));
        float  specP   = KajiyaKaySpec(T, Hp, shininess) * (1.0f - rough);
        result += (col * NdotLp + specColor * specP)
                * pointLights[pi].color * pointLights[pi].intensity * atten;
    }
    [loop] for (int si = 0; si < spotLightCount; ++si)
    {
        float3 toLight = spotLights[si].position - p.worldPos;
        float  dist    = length(toLight);
        float3 Ls      = toLight / dist;
        float3 Hs      = normalize(V + Ls);
        float  atten   = LightAttenuation(dist, spotLights[si].range);
        float  cone    = SpotConeWeight(Ls, spotLights[si].direction,
                             spotLights[si].innerCos, spotLights[si].outerCos);
        float  NdotLs  = saturate(dot(N, Ls));
        float  specS   = KajiyaKaySpec(T, Hs, shininess) * (1.0f - rough);
        result += (col * NdotLs + specColor * specS)
                * spotLights[si].color * spotLights[si].intensity * atten * cone;
    }

    // エミッシブ
    float3 emissiveTex = (textureMask & (1u << 3))
        ? texEmissive.Sample(sampDefault, uv).rgb
        : float3(1.0f, 1.0f, 1.0f);
    result += emissiveTex * emissiveColor * emissiveScale;

    return float4(result, alpha);
}
