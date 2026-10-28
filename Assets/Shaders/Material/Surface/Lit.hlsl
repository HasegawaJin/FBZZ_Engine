/// @file    Lit.hlsl
/// @brief   Lit.hlsl の既存描画と共通頂点契約。
/// @author  Hasegawa Jin
/// @date    2026-06-23

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
    float4 albedo;       /// @note RGBA ベースカラー  offset 0
    uint   textureMask;  /// @note テクスチャフラグ   offset 16

    /// @note bindless のテクスチャ添字。Material::Upload が毎フレーム書き込む。
    /// @note ここに宣言した枠だけが Inspector に出る (Common/MaterialTextures.hlsli)。
    uint texAlbedoIndex;
};

FBZZ_TEX2D_T(float, texShadow, TEX_SHADOW_SLOT);
FBZZ_MATERIAL_TEX(texAlbedo, texAlbedoIndex);
SamplerState           sampDefault  : register(SAMPLER_DEFAULT);
SamplerComparisonState sampShadow   : register(SAMPLER_SHADOW);

PSInput VSMain(VSInput v)
{
    return TransformMaterialVertex(v);
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
    /// @note Forward の画面空間 AO / 接触影。Deferred では b8 が 0 なので素通りする。
    shadow *= FBZZ_ScreenContactShadow(p.svPosition.xy);
    float3 result = Lighting_Lambert(N, L, col, lightColor, lightIntensity, shadow);

    /// @note 点光源 / スポットライト — 走査元は clusterLightMode が決める
    /// @note (b3 の固定長配列 / StructuredBuffer / クラスタリスト)。
    FBZZ_PUNCTUAL_BEGIN(p.worldPos, p.svPosition.xy, N)
        result += Lighting_Lambert_Direct(N, ps.L, col,
            ps.color, ps.intensity);
    FBZZ_PUNCTUAL_END
    return float4(result, 1.0f);
}