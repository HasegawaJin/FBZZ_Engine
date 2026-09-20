/// @file    PBR.hlsl
/// @brief   Cook-Torrance PBR のフォワードパス (法線マップ / AO / エミッシブ / PCF シャドウ)。
/// @author  Hasegawa Jin
/// @date    2026-05-19

#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Common/Color.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Common/MaterialTextures.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/Lighting.hlsli"
#include "Rendering/Shadow.hlsli"
#include "Rendering/SpecularAA.hlsli"
#include "Rendering/Wetness.hlsli"
#include "Rendering/LodDither.hlsli"
/// @note ApplyNormalMap は Shadow.hlsli → Space.hlsli 経由で入る。

/// @note offset は C++ 側 MaterialConstants と一致させる。
cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;             ///< RGBA ベースカラー (線形)。offset 0
    float  metallic;           ///< [0,1]。offset 16
    float  roughness;          ///< [0,1]。offset 20
    float  normalStrength;     ///< 0 でサーフェス法線。offset 24
    float  occlusionStrength;  ///< [0,1]。offset 28
    float3 emissiveColor;      ///< offset 32
    float  emissiveScale;      ///< 0 で非発光。offset 44
    float2 uvTiling;           ///< offset 48
    float2 uvOffset;           ///< offset 56
    float  alphaCutoff;        ///< offset 64
    float3 _pad0;              ///< offset 68
    uint   textureMask;        ///< bit0 albedo / 1 normal / 2 metalRough / 3 emissive / 4 AO。offset 80
    float3 _pad1;              ///< offset 84
    float  clearcoat;
    float  clearcoatRoughness;
    float  sheen;
    float  anisotropy;
    float3 sheenColor;
    float  _pad2;

    /// @note bindless のテクスチャ添字。Material::Upload が毎フレーム書き、ここに宣言した枠だけが Inspector に出る (Common/MaterialTextures.hlsli)。
    uint texAlbedoIndex;
    uint texNormalIndex;
    uint texMetallicIndex;
    uint texEmissiveIndex;
    uint texAOIndex;
};

/// @note bindless 化は宣言の置き換えだけで、Sample 呼び出しは従来のまま。添字は b14 の添字ブロックで毎ドロー配られる。
/// @see Docs/design/bindless.md
FBZZ_TEX2D_T(float,  texShadow,        TEX_SHADOW_SLOT);
FBZZ_MATERIAL_TEX(texAlbedo, texAlbedoIndex);
FBZZ_MATERIAL_TEX(texNormal, texNormalIndex);
FBZZ_MATERIAL_TEX(texMetallicRough, texMetallicIndex);
FBZZ_MATERIAL_TEX(texEmissive, texEmissiveIndex);
FBZZ_MATERIAL_TEX(texAO, texAOIndex);
FBZZ_TEXCUBE(        texIBLIrradiance, TEX_IBL_IRRADIANCE_SLOT);
FBZZ_TEXCUBE(        texIBLPrefilter,  TEX_IBL_PREFILTER_SLOT);
FBZZ_TEX2D_T(float4, texBRDFLut,       TEX_IBL_BRDF_LUT_SLOT);
/// @note サンプラーはルートシグネチャの静的サンプラーで、bindless の対象外。
SamplerState           sampDefault      : register(SAMPLER_DEFAULT);
SamplerComparisonState sampShadow       : register(SAMPLER_SHADOW);
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
    ApplyLodDither(p.svPosition.xy, objectParams.x);

    float2 uv = p.uv * uvTiling + uvOffset;

    /// @note sRGB を線形へ戻してから線形の tint を掛ける。テクスチャ無しは 1 で SRGBToLinear(1)=1 なので albedo がそのまま出る。
    float4 rawAlbedo = (textureMask & (1u << 0))
        ? texAlbedo.Sample(sampDefault, uv)
        : float4(1.0f, 1.0f, 1.0f, 1.0f);
    float3 col   = SRGBToLinear(rawAlbedo.rgb) * albedo.rgb;
    float  alpha = rawAlbedo.a * albedo.a;

    /// @note カットアウト。不透明パスなのでディザリングせず棄却する。
    clip(alpha - alphaCutoff);

    float3 N = SafeNormalize(p.normal, float3(0.0f, 1.0f, 0.0f));
    if (textureMask & (1u << 1))
    {
        float3 ns = texNormal.Sample(sampDefault, uv).rgb;
        float3 nm = ApplyNormalMap(ns, N, SafeNormalize(p.tangent, float3(1.0f, 0.0f, 0.0f)));
        N = SafeNormalize(lerp(N, nm, saturate(normalStrength)), N);
    }

    /// @note glTF 規約: G = roughness、B = metallic。
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
    /// @note 濡れは素材の値なので法線分散のフィルタより先に掛ける (GBuffer.hlsl と同順)。
    const WetSurface wet = ApplyWetness(col, rough, N);
    col   = wet.albedo;
    rough = max(wet.roughness, 0.045f);
    rough = FilterSpecularRoughness(N, rough);

    const float3 tangent = SafeNormalize(p.tangent, float3(1.0f, 0.0f, 0.0f));
    float3 T = SafeNormalize(tangent - N * dot(N, tangent),
                             abs(N.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f));
    T = SafeNormalize(T - N * dot(N, T), T);
    float3 B = SafeNormalize(cross(N, T), float3(0.0f, 0.0f, 1.0f));

    float ao = 1.0f;
    if (textureMask & (1u << 4))
        ao = lerp(1.0f, texAO.Sample(sampDefault, uv).r, occlusionStrength);

    float3 V      = SafeNormalize(cameraPos - p.worldPos, N);
    float3 L      = SafeNormalize(-lightDir, N);
    float  shadow = ComputeShadow(texShadow, sampShadow, p.worldPos,
                                  lightViewProjection, shadowMapTexelSize, shadowBias, N, L);
    /// @note Forward の画面空間 AO / 接触影。Deferred では b8 が 0 なので素通りする。
    shadow *= FBZZ_ScreenContactShadow(p.svPosition.xy);
    ao *= FBZZ_ScreenAO(p.svPosition.xy);
    float3 result = iblIntensity > 0.0f
        ? Lighting_PBR_IBL_Advanced(p.worldPos, N, V, L, T, B, col, met, rough,
              clearcoat, clearcoatRoughness, sheen, anisotropy, sheenColor,
              lightColor, lightIntensity, shadow, ao,
              texIBLIrradiance, texIBLPrefilter, texBRDFLut, iblMaxMipLevel,
              iblIntensity, iblDiffuseScale, iblSpecularScale,
              sampDefault, sampLinearClamp)
        : Lighting_PBR_Advanced(N, V, L, T, B, col, met, rough,
              clearcoat, clearcoatRoughness, sheen, anisotropy, sheenColor,
              lightColor, lightIntensity, shadow);

    /// @note 点光源 / スポットの走査元は clusterLightMode が決める (b3 の固定長配列 / StructuredBuffer / クラスタリスト)。
    FBZZ_PUNCTUAL_BEGIN(p.worldPos, p.svPosition.xy, N)
        result += Lighting_PBR_Advanced(N, V, ps.L, T, B, col, met, saturate(rough + ps.roughnessBias),
            clearcoat, clearcoatRoughness, sheen, anisotropy, sheenColor,
            ps.color, ps.intensity, 1.0f);
    FBZZ_PUNCTUAL_END

    /// @note エミッシブテクスチャは sRGB。
    float3 emissiveTex = (textureMask & (1u << 3))
        ? SRGBToLinear(texEmissive.Sample(sampDefault, uv).rgb)
        : float3(1.0f, 1.0f, 1.0f);
    result += emissiveTex * emissiveColor * emissiveScale;

    return float4(result, alpha);
}