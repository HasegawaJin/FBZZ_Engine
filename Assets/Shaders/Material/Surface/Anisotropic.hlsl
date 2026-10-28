/// @file    Anisotropic.hlsl
/// @brief   Anisotropic.hlsl の既存描画と共通頂点契約。
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

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;             /// @note RGBA ベースカラー              offset  0
    float  metallic;           /// @note 色付きスペキュラ強度 [0,1]      offset 16
    float  roughness;          /// @note スペキュラ広がり (低=鋭い)      offset 20
    float  normalStrength;     /// @note タンジェントシフト量            offset 24
    float  occlusionStrength;  /// @note AO 強度 [0,1]                  offset 28
    float3 emissiveColor;      /// @note エミッシブ色                   offset 32
    float  emissiveScale;      /// @note エミッシブ強度                 offset 44
    float2 uvTiling;           /// @note UV タイリング                  offset 48
    float2 uvOffset;           /// @note UV オフセット                  offset 56
    float  alphaCutoff;        /// @note アルファカットオフ               offset 64
    float3 _pad0;              /// @note offset 68
    uint   textureMask;        /// @note テクスチャフラグ                offset 80
    float3 _pad1;              /// @note offset 84

    /// @note bindless のテクスチャ添字。Material::Upload が毎フレーム書き込む。
    /// @note ここに宣言した枠だけが Inspector に出る (Common/MaterialTextures.hlsli)。
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

/// @note Kajiya-Kay 異方性スペキュラ項。
/// @note T  : ファイバー方向 (法線シフト済みタンジェント)
/// @note H  : ハーフベクトル
/// @note shininess : 集中度
float KajiyaKaySpec(float3 T, float3 H, float shininess)
{
    float TdotH = dot(T, H);
    float sinTH = sqrt(max(0.0f, 1.0f - TdotH * TdotH));
    return pow(sinTH, shininess);
}

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

    /// @note normalStrength でタンジェントを法線方向へシフト → ハイライト帯の縦位置を制御。
    float3 T = normalize(p.tangent + N * (normalStrength * 0.3f - 0.15f));

    float3 V      = normalize(cameraPos - p.worldPos);
    float3 L      = normalize(-lightDir);
    float3 H      = normalize(V + L);
    float  shadow = ComputeShadow(texShadow, sampShadow, p.worldPos,
                                  lightViewProjection, shadowMapTexelSize, shadowBias, N, L);
    /// @note Forward の画面空間 AO / 接触影。Deferred では b8 が 0 なので素通りする。
    shadow *= FBZZ_ScreenContactShadow(p.svPosition.xy);
    ao *= FBZZ_ScreenAO(p.svPosition.xy);

    float  NdotL      = saturate(dot(N, L));
    float  shininess  = max(lerp(512.0f, 2.0f, rough), 2.0f);
    float  specTerm   = KajiyaKaySpec(T, H, shininess);
    /// @note 金属度で色付きスペキュラ / 無彩色スペキュラを補間する。
    float3 specColor  = lerp(float3(1.0f, 1.0f, 1.0f), col, met);
    float3 specular   = specColor * lightColor * lightIntensity
                      * specTerm * (1.0f - rough) * shadow;

    float3 ambient    = col * 0.08f * ao;
    float3 diffuse    = col * lightColor * lightIntensity * NdotL * shadow;
    float3 result     = ambient + diffuse + specular;

    /// @note 点光源 / スポットライト — 走査元は clusterLightMode が決める
    /// @note (b3 の固定長配列 / StructuredBuffer / クラスタリスト)。
    FBZZ_PUNCTUAL_BEGIN(p.worldPos, p.svPosition.xy, N)
        float3 Hp     = normalize(V + ps.L);
        float  NdotLp = saturate(dot(N, ps.L));
        float  specP  = KajiyaKaySpec(T, Hp, shininess) * (1.0f - rough);
        result += (col * NdotLp + specColor * specP) * ps.color * ps.intensity;
    FBZZ_PUNCTUAL_END

    /// @note エミッシブ
    float3 emissiveTex = (textureMask & (1u << 3))
        ? texEmissive.Sample(sampDefault, uv).rgb
        : float3(1.0f, 1.0f, 1.0f);
    result += emissiveTex * emissiveColor * emissiveScale;

    return float4(result, alpha);
}