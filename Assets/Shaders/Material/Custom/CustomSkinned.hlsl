/// @file    CustomSkinned.hlsl
/// @brief   CustomSkinned.hlsl の既存描画と共通頂点契約。
/// @author  Hasegawa Jin
/// @date    2026-06-23

#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/MaterialVertex.hlsli"
#include "Common/Structs.hlsli"
#include "Common/Skinning.hlsli"
#include "Common/Color.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/Lighting.hlsli"
#include "Rendering/Shadow.hlsli"
#include "Rendering/SurfaceCommon.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Common/MaterialTextures.hlsli"

/// @note #define FBZZ_MATERIAL_CONSTANTS により Constants.hlsli の
/// @note デフォルト cbuffer は展開されない。ここで自分の変数を宣言する。
/// @note 変数名と型がそのまま Inspector の UI として自動生成される。
/// @note textureMask は必ず最後の 16-byte チャンクの先頭に置くこと。
/// @note Upload() が自動計算して書き込む (手動設定不要)。
cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;            ///< @note ベースカラー (RGBA)            offset  0
    float  roughness;         ///< @note 粗さ [0, 1] (鏡面ハイライト幅) offset 16
    float  normalStrength;    ///< @note 法線マップ強度 [0, 1]           offset 20
    float  emissiveScale;     ///< @note 自発光強度                      offset 24
    float  alphaCutoff;       ///< @note アルファカットオフ閾値 [0, 1]   offset 28
    float2 uvTiling;          ///< @note UV スケール (X, Y)              offset 32
    float2 uvOffset;          ///< @note UV オフセット (X, Y)            offset 40
    uint   textureMask;       ///< @note テクスチャ存在フラグ (自動設定) offset 48
    float3 _pad;              ///< @note offset 52

    /// @note bindless のテクスチャ添字。Material::Upload が毎フレーム書き込む。
    /// @note ここに宣言した枠だけが Inspector に出る (Common/MaterialTextures.hlsli)。
    uint texAlbedoIndex;
    uint texNormalIndex;
};

FBZZ_MATERIAL_TEX(texAlbedo, texAlbedoIndex);   ///< @note t0: ベースカラー
FBZZ_MATERIAL_TEX(texNormal, texNormalIndex);   ///< @note t1: 法線マップ (Tangent Space)
FBZZ_TEX2D_T(float, texShadow, TEX_SHADOW_SLOT);   ///< @note t8: シャドウマップ (RenderSystem が自動バインド)
SamplerState           sampDefault : register(SAMPLER_DEFAULT);
SamplerComparisonState sampShadow  : register(SAMPLER_SHADOW);

/// @note 4 本のボーンを線形ブレンドしてスキン行列を計算する。
/// @note SkinnedBlinnPhong.hlsl と同一ロジックを使い、エンジンのリグ規約に準拠する。

PSInput VSMain(SkinnedVSInput v)
{
    return TransformSafeSkinnedMaterialVertex(v);
}

/// @note ここから下を自由にカスタマイズしてください。
float4 PSMain(PSInput p) : SV_Target0
{
    float2 uv = SurfaceTransformUv(p.uv, uvTiling, uvOffset);

    /// @note Albedo: 共通 API がテクスチャ有無・sRGB・フォールバックを処理する。
    float4 surfaceAlbedo = SurfaceSampleAlbedo(texAlbedo, sampDefault, uv,
                                               albedo, textureMask);
    float3 col   = surfaceAlbedo.rgb;
    float  alpha = surfaceAlbedo.a;
    clip(alpha - alphaCutoff);

    /// @note 法線 (法線マップがあれば適用)
    float3 N = SurfaceSampleNormal(texNormal, sampDefault, uv, p.normal,
                                   p.tangent, normalStrength, textureMask);

    /// @note シャドウ
    float3 L      = SurfaceSafeNormalize(-lightDir, float3(0.0f, 1.0f, 0.0f));
    float  shadow = ComputeShadow(texShadow, sampShadow, p.worldPos,
                                  lightViewProjection, shadowMapTexelSize, shadowBias, N, L);
    /// @note スキンドメッシュは GBuffer に描かれない (ExecuteGBufferPass が isSkinned を除外)。
    /// @note 画面空間 AO / 接触影を引くと、キャラの画素で「背景の遮蔽」を読んでしまうので使わない。
    /// @note これは Deferred でも同じ (キャラは DeferredLighting を通らない) ため、差は生じない。

    /// @note Blinn-Phong ライティング
    float3 V        = SurfaceSafeNormalize(cameraPos - p.worldPos, N);
    float3 H        = SurfaceSafeNormalize(L + V, N);
    float  NdotL    = max(0.0f, dot(N, L));
    float  NdotH    = max(0.0f, dot(N, H));
    float  shininess = max(1.0f, (1.0f - roughness) * 128.0f);

    float3 ambient  = ambientColor * col;
    float3 diffuse  = lightColor * lightIntensity * NdotL * shadow * col;
    float3 specular = lightColor * lightIntensity * pow(NdotH, shininess) * shadow * 0.5f;
    float3 result   = ambient + diffuse + specular;

    /// @note ポイントライト (点光源)
    /// @note 点光源 / スポットライト — 走査元は clusterLightMode が決める
    /// @note (b3 の固定長配列 / StructuredBuffer / クラスタリスト)。
    FBZZ_PUNCTUAL_BEGIN(p.worldPos, p.svPosition.xy, N)
        float3 Hp  = SurfaceSafeNormalize(ps.L + V, N);
        float  dif = max(0.0f, dot(N, ps.L));
        float  spe = pow(max(0.0f, dot(N, Hp)), shininess) * 0.5f;
        result += ps.color * ps.intensity * (dif * col + spe);
    FBZZ_PUNCTUAL_END

    /// @note 自発光
    result += col * emissiveScale;

    return float4(result, alpha);
}
