/// @file    ShadowConstants.hlsli
/// @brief   ShadowConstants の共通定数バッファ。
/// @author  Hasegawa Jin
/// @date    2026-08-14

/// @note 以前は Constants.hlsli / Terrain.hlsl / Water.hlsl が同じレイアウトを 3 か所へ
/// @note 手書きしていた。カスケードシャドウでメンバーが増えると、1 か所だけ直し忘れた
/// @note シェーダーが黙って別のオフセットを読む (影が消える・座標がずれる) 事故になる。
/// @note 定義をここへ集約し、C++ の ShadowConstantsCB と 1 対 1 で対応させる。

/// @note Graphics/Pipeline/RenderConstants.hpp の
/// @note ShadowConstantsCB と完全に一致させること (512 bytes / 32 レジスタ)。
/// @note C++ 側に static_assert(sizeof == 512) を置いてある。
#ifndef SHADOW_CONSTANTS_HLSLI
#define SHADOW_CONSTANTS_HLSLI

#include "Common/Binding.hlsli"

/// @note カスケード最大数。C++ の kMaxShadowCascades と一致させること。
#define FBZZ_MAX_SHADOW_CASCADES 4

cbuffer ShadowConstants : register(CB_SHADOW)
{
    /// @note 単一のライト行列で足りるパス向け (= cascadeViewProjection[0] と同じ内容)。
    /// @note パーティクル自己影・体積光・リフレクションプローブは「カスケード」という概念を
    /// @note 持たず、1 本の光源行列だけを必要とする。カスケード配列とは別に置いておくことで、
    /// @note それらのシェーダーが分割数を意識せずに済む。
    float4x4 lightViewProjection;

    /// @note カスケードごとのライト viewProjection。有効なのは先頭 cascadeCount 本。
    float4x4 cascadeViewProjection[FBZZ_MAX_SHADOW_CASCADES];

    /// @note カスケードごとのアトラス矩形。xy = UV オフセット, zw = UV スケール。
    /// @note 全カスケードは 1 枚の深度テクスチャを 2x2 に区切って共有する。
    /// @note サンプル側は [0,1] のカスケード内 UV をこの矩形へ写してからテクスチャを引く。
    float4   cascadeAtlasRect[FBZZ_MAX_SHADOW_CASCADES];

    /// @note カスケードごとの NDC 深度バイアス (x=cascade0 .. w=cascade3)。
    /// @note カスケードごとに正射影の深度レンジが違うので、同じワールド距離のオフセットでも
    /// @note NDC 換算値が変わる。1 つの値を共有するとどこかで必ずアクネか Peter Panning が出る。
    float4   cascadeBias;

    /// @note 1.0 / アトラス全体の解像度
    float2   shadowMapTexelSize;
    /// @note 単一カスケード時のバイアス (= cascadeBias.x)
    float    shadowBias;
    /// @note 0=影なし, 1=完全な影
    float    shadowStrength;

    /// @note PCF カーネル半径: 0=ハード, 1=3x3, 2=5x5, 3=7x7
    int      shadowPcfRadius;
    /// @note 1 = 単一シャドウマップ (従来), 2〜4 = CSM
    int      cascadeCount;
    /// @note カスケード境界のクロスフェード幅 [0,1]。0 で境界が硬い
    float    cascadeBlend;
    /// @note 1 = カスケード番号を色で可視化する (デバッグ)
    int      cascadeDebugView;

    /// @note 雲シャドウ (Phase C)
    /// @note 0=無効
    float    cloudShadowStrength;
    float    cloudShadowCoverage;
    float    cloudShadowScale;
    float    cloudShadowSpeed;

    float    cloudShadowTime;
    float    cloudShadowWindX;
    float    cloudShadowWindZ;
    float    _shadowPad0;
    float4   cascadeSplitFar;
    float4   shadowCameraPosition;
    float4   shadowCameraForward;
};

/// @note 雲シャドウ / カスケードのフィールドを CB から読める目印。
/// @note Shadow.hlsli がフォールバックの static const 群へ落ちるかどうかを切り替える。
#define HAVE_CLOUD_SHADOW 1
#define HAVE_SHADOW_CASCADES 1

/// @note SHADOW_CONSTANTS_HLSLI
#endif
