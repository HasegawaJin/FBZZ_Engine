/// @file    SkinnedSubsurface.hlsl
/// @brief   SkinnedSubsurface.hlsl の既存描画と共通頂点契約。
/// @author  Hasegawa Jin
/// @date    2026-05-28

#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Common/MaterialVertex.hlsli"
#include "Common/Skinning.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/Lighting.hlsli"
#include "Rendering/Shadow.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Common/MaterialTextures.hlsli"

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



PSInput VSMain(SkinnedVSInput v)
{
    return TransformSkinnedMaterialVertex(v);
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
    /// @note スキンドメッシュは GBuffer に描かれない (ExecuteGBufferPass が isSkinned を除外)。
    /// @note 画面空間 AO / 接触影を引くと、キャラの画素で「背景の遮蔽」を読んでしまうので使わない。
    /// @note これは Deferred でも同じ (キャラは DeferredLighting を通らない) ため、差は生じない。

    float  wrap       = roughness * 0.8f;
    float  NdotL_wrap = saturate((dot(N, L) + wrap) / ((1.0f + wrap) * (1.0f + wrap)));
    float3 ambient    = col * 0.08f * ao;
    float3 diffuse    = col * lightColor * lightIntensity * NdotL_wrap * shadow;

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