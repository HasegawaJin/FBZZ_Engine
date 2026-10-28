/// @file    SkinnedEyeSprite.hlsl
/// @brief   SkinnedEyeSprite.hlsl の既存描画と共通頂点契約。
/// @author  Hasegawa Jin
/// @date    2026-08-19

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

/// @note 変数名と型がそのまま Inspector / .mat の [params] キーになる。
/// @note albedo / emissiveColor / emissiveScale / uvTiling / uvOffset / alphaCutoff は
/// @note エンジン標準レイアウトと同じ名前・同じオフセットに揃えてある。
/// @note InitDefaultMaterialParams が名前で既定値を書き込むため、独自名にすると
/// @note 「.mat に書いていない項目が 0 で始まる」という材質ごとの差が生まれる。
/// @note textureMask は必ず最後の 16-byte チャンクの先頭に置くこと。
/// @note Material::Upload() が自動計算して書き込む (.mat での手動設定は不要)。
cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;         ///< @note offset  0: 目の色。テクスチャがあれば乗算 tint として働く
    float3 emissiveColor;  ///< @note offset 16: 自発光の色。バイザーの発光色をここで決める
    float  emissiveScale;  ///< @note offset 28: 自発光の強さ。0 で消灯
    float  shadeStrength;  ///< @note offset 32: 陰影の効き [0,1]。0 = 完全に平坦 / 1 = 陰影あり
    float  ambientFloor;   ///< @note offset 36: 影の中でも保つ最低輝度 [0,1]。暗所で目が消えるのを防ぐ
    float  alphaCutoff;    ///< @note offset 40: これ未満の α を捨てる。0 なら切り抜きなし
    float  _pad0;          ///< @note offset 44
    float2 uvTiling;       ///< @note offset 48: UV スケール (Sprite 矩形が自動合成される)
    float2 uvOffset;       ///< @note offset 56: UV オフセット (同上)
    uint   textureMask;    ///< @note offset 64: テクスチャ存在フラグ (自動設定)
    float3 _pad1;          ///< @note offset 68

    /// @note bindless のテクスチャ添字。Material::Upload が毎フレーム書き込む。
    /// @note ここに宣言した枠だけが Inspector に出る (Common/MaterialTextures.hlsli)。
    uint texAlbedoIndex;
};

FBZZ_MATERIAL_TEX(texAlbedo, texAlbedoIndex);   ///< @note t0: 目のスプライト
FBZZ_TEX2D_T(float, texShadow, TEX_SHADOW_SLOT);   ///< @note t8: シャドウマップ (自動バインド)
SamplerState           sampDefault : register(SAMPLER_DEFAULT);
SamplerComparisonState sampShadow  : register(SAMPLER_SHADOW);

/// @note 4 本のボーンを線形ブレンドしてスキン行列を作る。
/// @note 他の Skinned シェーダーと完全に同一のロジックにしておく。ここだけ独自の
/// @note ブレンドにすると、顔と目が別々に変形して目が顔から外れる。

PSInput VSMain(SkinnedVSInput v)
{
    return TransformSafeSkinnedMaterialVertex(v);
}

float4 PSMain(PSInput p) : SV_Target0
{
    /// @note Sprite 矩形が合成済みの UV 変換。atlas の 1 コマだけをここで切り出す。
    float2 uv = SurfaceTransformUv(p.uv, uvTiling, uvOffset);

    float4 surfaceAlbedo = SurfaceSampleAlbedo(texAlbedo, sampDefault, uv,
                                               albedo, textureMask);
    float3 col   = surfaceAlbedo.rgb;
    float  alpha = surfaceAlbedo.a;
    /// @note しきい値 0 を分けるか: alphaCutoff = 0 のまま clip(alpha) を通すと、
    /// @note α がちょうど 0 のピクセルだけがドライバ依存で落ちる。「切り抜かない」を
    /// @note 選んだ材質では clip 自体を通さず、挙動をバックエンド間で一致させる。
    if (alphaCutoff > 0.0f)
        clip(alpha - alphaCutoff);

    float3 N = SurfaceSafeNormalize(p.normal, float3(0.0f, 1.0f, 0.0f));
    float3 L = SurfaceSafeNormalize(-lightDir, float3(0.0f, 1.0f, 0.0f));

    float shadow = ComputeShadow(texShadow, sampShadow, p.worldPos,
                                 lightViewProjection, shadowMapTexelSize,
                                 shadowBias, N, L);
    /// @note スキンドメッシュは GBuffer に描かれない (ExecuteGBufferPass が isSkinned を除外)。
    /// @note 画面空間 AO / 接触影を引くと、キャラの画素で「背景の遮蔽」を読んでしまうので使わない。
    /// @note これは Deferred でも同じ (キャラは DeferredLighting を通らない) ため、差は生じない。

    /// @note ハーフランバート。目は球面に近い形状なので素の N·L だと縁が黒く落ち、
    /// @note 白目と背景の境界が潰れて表情が読めなくなる。

    /// @note 放射照度 (lightIntensity・LIGHT_UNIT_SCALE) を通さないか:
    /// @note ここで欲しいのは明るさそのものではなく「元の絵をどこまで暗くしていいか」
    /// @note という 0〜1 の減光係数である。物理単位の強度を掛けると屋外の太陽
    /// @note (intensity > 1) で係数が 1 を超えて描き込み済みの絵が白飛びし、
    /// @note ambientFloor で守っているはずの下限と一緒に上限まで壊れる。
    /// @note 目の見た目をシーンの明るさから切り離すのは、この材質の狙いそのもの。
    float3 shaded = lightColor * (saturate(dot(N, L) * 0.5f + 0.5f) * shadow);

    /// @note 陰影の効き。shadeStrength = 0 なら完全に平坦な 2D として出る。
    float3 lighting = lerp(float3(1.0f, 1.0f, 1.0f), shaded, saturate(shadeStrength));
    /// @note 影・夜・光源色のどれで暗くなった場合でも、ここが最終的な下限を保証する。
    lighting = max(lighting, saturate(ambientFloor).xxx);

    float3 result = col * lighting;

    /// @note 点光源 / スポットライト — 走査元は clusterLightMode が決める
    /// @note (b3 の固定長配列 / StructuredBuffer / クラスタリスト)。
    /// @note 目は自発光が主体なので、寄与は shadeStrength で絞った分だけ足す。
    FBZZ_PUNCTUAL_BEGIN(p.worldPos, p.svPosition.xy, N)
        float punctual = saturate(dot(N, ps.L) * 0.5f + 0.5f);
        result += col * ps.color * ps.intensity * punctual * saturate(shadeStrength);
    FBZZ_PUNCTUAL_END

    /// @note 自発光。影も光源も掛けない。
    /// @note バイザーや瞳のハイライトは「自分で光っている面」なので、影に入った
    /// @note 瞬間に消えると発光しているように見えなくなる。ここが shadow を
    /// @note 無視することで、暗所でも目の位置と表情が必ず読める。
    result += col * emissiveColor * emissiveScale;

    return float4(result, alpha);
}
