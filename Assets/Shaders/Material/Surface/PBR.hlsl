// FBZZ Engine
// Material/Surface/PBR.hlsl | Material
// Cook-Torrance PBR フォワードパス (法線マップ / AO / エミッシブ / PCF シャドウ)

#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Common/Color.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/Lighting.hlsli"
#include "Rendering/Shadow.hlsli"
// ApplyNormalMap は Shadow.hlsli → Space.hlsli 経由で提供される

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;             // RGBA ベースカラー    offset  0
    float  metallic;           // 金属度 [0,1]         offset 16
    float  roughness;          // 粗さ   [0,1]         offset 20
    float  normalStrength;     // 法線マップ強度        offset 24
    float  occlusionStrength;  // AO 強度 [0,1]        offset 28
    float3 emissiveColor;      // エミッシブ色          offset 32
    float  emissiveScale;      // エミッシブ強度        offset 44
    float2 uvTiling;           // UV タイリング        offset 48
    float2 uvOffset;           // UV オフセット        offset 56
    float  alphaCutoff;        // アルファカットオフ     offset 64
    float3 _pad0;              //                      offset 68
    uint   textureMask;        // テクスチャフラグ       offset 80
    float3 _pad1;              //                      offset 84
    float  clearcoat;
    float  clearcoatRoughness;
    float  sheen;
    float  anisotropy;
    float3 sheenColor;
    float  _pad2;
};

Texture2D<float>       texShadow        : register(TEX_SHADOW);
Texture2D              texAlbedo        : register(TEX_ALBEDO);
Texture2D              texNormal        : register(TEX_NORMAL);
Texture2D              texMetallicRough : register(TEX_METALLIC_ROUGH);
Texture2D              texEmissive      : register(TEX_EMISSIVE);
Texture2D              texAO            : register(TEX_AO);
SamplerState           sampDefault      : register(SAMPLER_DEFAULT);
SamplerComparisonState sampShadow       : register(SAMPLER_SHADOW);
TextureCube            texIBLIrradiance : register(TEX_IBL_IRRADIANCE);
TextureCube            texIBLPrefilter  : register(TEX_IBL_PREFILTER);
Texture2D<float4>      texBRDFLut       : register(TEX_IBL_BRDF_LUT);
SamplerState           sampLinearClamp  : register(SAMPLER_LINEAR_CLAMP);

PSInput VSMain(VSInput v)
{
    PSInput o;
    float4 worldPos4 = mul(float4(v.position, 1.0f), world);
    o.worldPos   = worldPos4.xyz;
    o.svPosition = mul(worldPos4, viewProjection);
    o.normal     = SafeNormalize(mul(v.normal,  (float3x3)worldInvTranspose), float3(0.0f, 1.0f, 0.0f));
    o.tangent    = SafeNormalize(mul(v.tangent, (float3x3)world), float3(1.0f, 0.0f, 0.0f));
    o.uv         = v.uv;
    return o;
}

float4 PSMain(PSInput p) : SV_Target0
{
    // UV タイリング / オフセットをすべてのサンプルに適用する。
    float2 uv = p.uv * uvTiling + uvOffset;

    // Albedo + alpha
    // sRGB テクスチャを線形空間にデコードしてから tint (線形) を乗算する。
    // テクスチャなし時は (1,1,1) として albedo.rgb をそのまま使用 (SRGBToLinear(1)=1)。
    float4 rawAlbedo = (textureMask & (1u << 0))
        ? texAlbedo.Sample(sampDefault, uv)
        : float4(1.0f, 1.0f, 1.0f, 1.0f);
    float3 col   = SRGBToLinear(rawAlbedo.rgb) * albedo.rgb;
    float  alpha = rawAlbedo.a * albedo.a;

    // alphaCutoff: カットアウト描画。不透明パスでディザリングなし早期棄却。
    clip(alpha - alphaCutoff);

    // Normal
    float3 N = SafeNormalize(p.normal, float3(0.0f, 1.0f, 0.0f));
    if (textureMask & (1u << 1))
    {
        float3 ns = texNormal.Sample(sampDefault, uv).rgb;
        float3 nm = ApplyNormalMap(ns, N, SafeNormalize(p.tangent, float3(1.0f, 0.0f, 0.0f)));
        // normalStrength=0 でサーフェス法線に戻る線形ブレンド。
        N = SafeNormalize(lerp(N, nm, saturate(normalStrength)), N);
    }

    // Metallic / Roughness (glTF 規約: G チャンネル = roughness, B チャンネル = metallic)
    float met   = metallic;
    float rough = roughness;
    if (textureMask & (1u << 2))
    {
        float2 mr = texMetallicRough.Sample(sampDefault, uv).gb;
        rough = mr.x;
        met   = mr.y;
    }

    met   = saturate(met);
    rough = max(saturate(rough), 0.045f);

    const float3 tangent = SafeNormalize(p.tangent, float3(1.0f, 0.0f, 0.0f));
    float3 T = SafeNormalize(tangent - N * dot(N, tangent),
                             abs(N.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f));
    T = SafeNormalize(T - N * dot(N, T), T);
    float3 B = SafeNormalize(cross(N, T), float3(0.0f, 0.0f, 1.0f));

    // AO
    float ao = 1.0f;
    if (textureMask & (1u << 4))
        ao = lerp(1.0f, texAO.Sample(sampDefault, uv).r, occlusionStrength);

    float3 V      = SafeNormalize(cameraPos - p.worldPos, N);
    float3 L      = SafeNormalize(-lightDir, N);
    float  shadow = ComputeShadow(texShadow, sampShadow, p.worldPos,
                                  lightViewProjection, shadowMapTexelSize, shadowBias, N, L);
    float3 result = iblIntensity > 0.0f
        ? Lighting_PBR_IBL_Advanced(N, V, L, T, B, col, met, rough,
              clearcoat, clearcoatRoughness, sheen, anisotropy, sheenColor,
              lightColor, lightIntensity, shadow, ao,
              texIBLIrradiance, texIBLPrefilter, texBRDFLut, iblMaxMipLevel,
              iblIntensity, iblDiffuseScale, iblSpecularScale,
              sampDefault, sampLinearClamp)
        : Lighting_PBR_Advanced(N, V, L, T, B, col, met, rough,
              clearcoat, clearcoatRoughness, sheen, anisotropy, sheenColor,
              lightColor, lightIntensity, shadow);

    [loop] for (int pi = 0; pi < pointLightCount; ++pi)
    {
        float3 toLight = pointLights[pi].position - p.worldPos;
        float  dist    = length(toLight);
        float3 Lp      = SafeNormalize(toLight, N);
        float  atten   = LightAttenuation(dist, pointLights[pi].range);
        result += Lighting_PBR_Advanced(N, V, Lp, T, B, col, met, rough,
                      clearcoat, clearcoatRoughness, sheen, anisotropy, sheenColor,
                      pointLights[pi].color, pointLights[pi].intensity * atten, 1.0f);
    }
    [loop] for (int si = 0; si < spotLightCount; ++si)
    {
        float3 toLight = spotLights[si].position - p.worldPos;
        float  dist    = length(toLight);
        float3 Ls      = SafeNormalize(toLight, N);
        float  atten   = LightAttenuation(dist, spotLights[si].range);
        float  cone    = SpotConeWeight(Ls, spotLights[si].direction,
                             spotLights[si].innerCos, spotLights[si].outerCos);
        result += Lighting_PBR_Advanced(N, V, Ls, T, B, col, met, rough,
                      clearcoat, clearcoatRoughness, sheen, anisotropy, sheenColor,
                      spotLights[si].color, spotLights[si].intensity * atten * cone, 1.0f);
    }

    // Emissive: テクスチャがあれば sRGB デコードして乗算。emissiveScale=0 で非発光。
    float3 emissiveTex = (textureMask & (1u << 3))
        ? SRGBToLinear(texEmissive.Sample(sampDefault, uv).rgb)
        : float3(1.0f, 1.0f, 1.0f);
    result += emissiveTex * emissiveColor * emissiveScale;

    return float4(result, alpha);
}
