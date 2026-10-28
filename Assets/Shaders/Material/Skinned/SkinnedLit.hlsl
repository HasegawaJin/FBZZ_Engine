/// @file    SkinnedLit.hlsl
/// @brief   SkinnedLit.hlsl の既存描画と共通頂点契約。
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
    uint   textureMask;

    /// @note bindless のテクスチャ添字。Material::Upload が毎フレーム書き込む。
    /// @note ここに宣言した枠だけが Inspector に出る (Common/MaterialTextures.hlsli)。
    uint texAlbedoIndex;
};

FBZZ_TEX2D_T(float, texShadow, TEX_SHADOW_SLOT);
FBZZ_MATERIAL_TEX(texAlbedo, texAlbedoIndex);
SamplerState           sampDefault : register(SAMPLER_DEFAULT);
SamplerComparisonState sampShadow  : register(SAMPLER_SHADOW);



PSInput VSMain(SkinnedVSInput v)
{
    return TransformSkinnedMaterialVertex(v);
}

float4 PSMain(PSInput p) : SV_Target0
{
    float3 col    = (textureMask & 1u)
        ? texAlbedo.Sample(sampDefault, p.uv).rgb
        : albedo.rgb;
    float3 N      = normalize(p.normal);
    float3 L      = normalize(-lightDir);
    float  shadow = ComputeShadow(texShadow, sampShadow, p.worldPos,
                                  lightViewProjection, shadowMapTexelSize, shadowBias, N, L);
    /// @note スキンドメッシュは GBuffer に描かれない (ExecuteGBufferPass が isSkinned を除外)。
    /// @note 画面空間 AO / 接触影を引くと、キャラの画素で「背景の遮蔽」を読んでしまうので使わない。
    /// @note これは Deferred でも同じ (キャラは DeferredLighting を通らない) ため、差は生じない。
    float3 result = Lighting_Lambert(N, L, col, lightColor, lightIntensity, shadow);

    /// @note 点光源 / スポットライト — 走査元は clusterLightMode が決める
    /// @note (b3 の固定長配列 / StructuredBuffer / クラスタリスト)。
    FBZZ_PUNCTUAL_BEGIN(p.worldPos, p.svPosition.xy, N)
        result += Lighting_Lambert_Direct(N, ps.L, col,
            ps.color, ps.intensity);
    FBZZ_PUNCTUAL_END
    return float4(result, 1.0f);
}