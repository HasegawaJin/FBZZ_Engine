// FBZZ Engine
// Common/ShadowConstants.hlsli | Common
// ShadowConstants (b4) cbuffer の単一定義
//
// WHY: 以前は Constants.hlsli / Terrain.hlsl / Water.hlsl が同じレイアウトを 3 か所へ
//      手書きしていた。カスケードシャドウでメンバーが増えると、1 か所だけ直し忘れた
//      シェーダーが黙って別のオフセットを読む (影が消える・座標がずれる) 事故になる。
//      定義をここへ集約し、C++ の ShadowConstantsCB と 1 対 1 で対応させる。
//
// LAYOUT: Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp の
//         ShadowConstantsCB と完全に一致させること (464 bytes / 29 レジスタ)。
//         C++ 側に static_assert(sizeof == 464) を置いてある。
#ifndef SHADOW_CONSTANTS_HLSLI
#define SHADOW_CONSTANTS_HLSLI

#include "Common/Binding.hlsli"

// カスケード最大数。C++ の kMaxShadowCascades と一致させること。
#define FBZZ_MAX_SHADOW_CASCADES 4

cbuffer ShadowConstants : register(CB_SHADOW)
{
    // 単一のライト行列で足りるパス向け (= cascadeViewProjection[0] と同じ内容)。
    // WHY: パーティクル自己影・体積光・リフレクションプローブは「カスケード」という概念を
    //      持たず、1 本の光源行列だけを必要とする。カスケード配列とは別に置いておくことで、
    //      それらのシェーダーが分割数を意識せずに済む。
    float4x4 lightViewProjection;

    // カスケードごとのライト viewProjection。有効なのは先頭 cascadeCount 本。
    float4x4 cascadeViewProjection[FBZZ_MAX_SHADOW_CASCADES];

    // カスケードごとのアトラス矩形。xy = UV オフセット, zw = UV スケール。
    // WHY: 全カスケードは 1 枚の深度テクスチャを 2x2 に区切って共有する。
    //      サンプル側は [0,1] のカスケード内 UV をこの矩形へ写してからテクスチャを引く。
    float4   cascadeAtlasRect[FBZZ_MAX_SHADOW_CASCADES];

    // カスケードごとの NDC 深度バイアス (x=cascade0 .. w=cascade3)。
    // WHY: カスケードごとに正射影の深度レンジが違うので、同じワールド距離のオフセットでも
    //      NDC 換算値が変わる。1 つの値を共有するとどこかで必ずアクネか Peter Panning が出る。
    float4   cascadeBias;

    float2   shadowMapTexelSize;  // 1.0 / アトラス全体の解像度
    float    shadowBias;          // 単一カスケード時のバイアス (= cascadeBias.x)
    float    shadowStrength;      // 0=影なし, 1=完全な影

    int      shadowPcfRadius;     // PCF カーネル半径: 0=ハード, 1=3x3, 2=5x5, 3=7x7
    int      cascadeCount;        // 1 = 単一シャドウマップ (従来), 2〜4 = CSM
    float    cascadeBlend;        // カスケード境界のクロスフェード幅 [0,1]。0 で境界が硬い
    int      cascadeDebugView;    // 1 = カスケード番号を色で可視化する (デバッグ)

    // 雲シャドウ (Phase C)
    float    cloudShadowStrength; // 0=無効
    float    cloudShadowCoverage;
    float    cloudShadowScale;
    float    cloudShadowSpeed;

    float    cloudShadowTime;
    float    cloudShadowWindX;
    float    cloudShadowWindZ;
    float    _shadowPad0;
};

// 雲シャドウ / カスケードのフィールドを CB から読める目印。
// Shadow.hlsli がフォールバックの static const 群へ落ちるかどうかを切り替える。
#define HAVE_CLOUD_SHADOW 1
#define HAVE_SHADOW_CASCADES 1

#endif // SHADOW_CONSTANTS_HLSLI
