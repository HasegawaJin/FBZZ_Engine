/// @file    Subsurface.hlsl
/// @brief   Subsurface.hlsl の既存描画と共通頂点契約。
/// @author  Hasegawa Jin
/// @date    2026-05-28

#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Common/MaterialVertex.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/Lighting.hlsli"
#include "Rendering/Shadow.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Common/MaterialTextures.hlsli"

/// @note SSS で使うパラメータのみ宣言 (metallic は不要)
cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;             /// @note RGBA ベースカラー            offset  0
    float  roughness;          /// @note ラップ量制御 (高=光が回り込む) offset 16
    float  normalStrength;     /// @note 法線マップ強度                offset 20
    float  occlusionStrength;  /// @note AO ブレンド強度              offset 24
    float  alphaCutoff;        /// @note アルファカットオフ             offset 28
    float3 emissiveColor;      /// @note SSS 散乱色                   offset 32
    float  emissiveScale;      /// @note SSS 散乱強度                 offset 44
    float2 uvTiling;           /// @note UV タイリング                offset 48
    float2 uvOffset;           /// @note UV オフセット                offset 56
    uint   textureMask;        /// @note テクスチャフラグ              offset 64

    /// @note bindless のテクスチャ添字。Material::Upload が毎フレーム書き込む。
    /// @note ここに宣言した枠だけが Inspector に出る (Common/MaterialTextures.hlsli)。
    uint texAlbedoIndex;
    uint texNormalIndex;
    uint texAOIndex;
};

FBZZ_TEX2D_T(float, texShadow, TEX_SHADOW_SLOT);
FBZZ_MATERIAL_TEX(texAlbedo, texAlbedoIndex);
FBZZ_MATERIAL_TEX(texNormal, texNormalIndex);
FBZZ_MATERIAL_TEX(texAO, texAOIndex);
SamplerState           sampDefault : register(SAMPLER_DEFAULT);
SamplerComparisonState sampShadow  : register(SAMPLER_SHADOW);

PSInput VSMain(VSInput v)
{
    return TransformMaterialVertex(v);
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
    /// @note Forward の画面空間 AO / 接触影。Deferred では b8 が 0 なので素通りする。
    shadow *= FBZZ_ScreenContactShadow(p.svPosition.xy);
    ao *= FBZZ_ScreenAO(p.svPosition.xy);

    /// @note ラップ拡散: roughness が高いほど光が影側に回り込む。
    /// @note wrap=0 で通常 Lambert、wrap=1 で半球全体がほぼ均等に照らされる。
    float  wrap        = roughness * 0.8f;
    float  NdotL_wrap  = saturate((dot(N, L) + wrap) / ((1.0f + wrap) * (1.0f + wrap)));
    float3 ambient     = col * 0.08f * ao;
    float3 diffuse     = col * lightColor * lightIntensity * NdotL_wrap * shadow;

    /// @note ビュー依存バックスキャター: カメラ-ライト-サーフェス が直線に近いほど強い透過光。
    /// @note 薄い素材 (耳、手など) で裏側からの透過光を模倣する。
    float  backScatter = pow(saturate(dot(V, -L)), 2.5f) * shadow;
    float3 scatter     = emissiveColor * emissiveScale
                       * (backScatter + 0.15f)
                       * lightColor * lightIntensity * ao;

    float3 result = ambient + diffuse + scatter;

    /// @note 点光源 / スポットライト — 走査元は clusterLightMode が決める
    /// @note (b3 の固定長配列 / StructuredBuffer / クラスタリスト)。
    FBZZ_PUNCTUAL_BEGIN(p.worldPos, p.svPosition.xy, N)
        float ndotlp = saturate((dot(N, ps.L) + wrap) / ((1.0f + wrap) * (1.0f + wrap)));
        result += col * ps.color * ps.intensity * ndotlp;
    FBZZ_PUNCTUAL_END

    return float4(result, alpha);
}