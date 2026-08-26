/// @file AdvancedGraphicsConstants.hlsli
/// @brief AdvancedGraphicsConstants (b8) cbuffer の単一定義
/// @author Hasegawa Jin
/// @date 2026-08-25
//
// WHY 独立したファイルにするか: Constants.hlsli を include できないシェーダー
//     (Terrain / Water — b0 / b1 / b3 を自前のレイアウトで使う) も b8 を読む必要がある。
//     以前は Water が先頭 8 フィールドだけを手書きで複製しており、末尾へフィールドを
//     足しても Water からは永久に見えなかった。ShadowConstants.hlsli と同じ判断で、
//     定義を 1 か所へ集約する。
//
// LAYOUT: Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp の
//         AdvancedGraphicsCB と完全に一致させること (352 bytes)。
#ifndef ADVANCED_GRAPHICS_CONSTANTS_HLSLI
#define ADVANCED_GRAPHICS_CONSTANTS_HLSLI

#include "Common/Binding.hlsli"

// AdvancedGraphicsConstants — IBL・SSR・TAA・GTAO・Contact Shadow 等の詳細グラフィクス設定。
// WHY: PostProcConstants は既存ポストプロセス設定で埋まっているため、
//      新規グラフィクス機能を独立した CB にまとめて管理しやすくする。
// LAYOUT: 全フィールドは 16-byte アライメントを維持し、DX11 CB パッキング規則に従う。
cbuffer AdvancedGraphicsConstants : register(CB_ADVANCED_GRAPHICS)
{
    // IBL (Image-Based Lighting)
    float  iblIntensity;        // 全体スケール
    float  iblDiffuseScale;     // 拡散 IBL スケール
    float  iblSpecularScale;    // 鏡面 IBL スケール
    int    iblMaxMipLevel;      // prefilter キューブマップの最大 mip レベル

    // SSR (Screen Space Reflections)
    float  ssrMaxDistance;      // 最大レイ距離
    float  ssrThickness;        // 深度交差判定厚み
    int    ssrSteps;            // レイマーチステップ数
    float  ssrIntensity;        // HDR への合成強度

    // Volumetric Lighting
    float  volLightIntensity;   // 光柱の明るさ
    float  volScattering;       // 散乱係数 g (Henyey-Greenstein)
    int    volSteps;            // レイマーチステップ数
    float  volMaxDist;          // レイマーチ最大距離

    // TAA (Temporal Anti-Aliasing)
    float  taaFeedback;         // 前フレームブレンド比 (0=無効, 0.9=標準)
    float  taaJitterX;          // 現フレーム Halton ジッター X
    float  taaJitterY;          // 現フレーム Halton ジッター Y
    float  _taaPad;

    // Motion Blur
    // screenWidth/screenHeight: b5 (PostProcConstants) が CS にバインドされないため
    //   MotionBlur CS の境界チェック・UV 計算用に CB_ADVANCED_GRAPHICS へ収録する
    float  motionBlurStrength;  // シャッター角度換算の強度
    int    motionBlurSamples;   // サンプル数
    float  screenWidth;         // レンダーターゲット幅 (CS スレッド境界チェック用)
    float  screenHeight;        // レンダーターゲット高さ (CS スレッド境界チェック用)

    // GTAO (Ground Truth Ambient Occlusion — Horizon-Based AO)
    float  gtaoIntensity;       // AO 強度
    float  gtaoRadius;          // サンプリング半径 (world space)
    int    gtaoSlices;          // 積分スライス数
    int    gtaoStepsPerSlice;   // スライスあたりステップ数

    // Contact Shadows
    float  contactShadowStrength;
    float  contactShadowRayLen; // レイ長さ (world space)
    int    contactShadowSteps;
    float  contactShadowThick;  // 深度比較用厚み

    // Lens Flare
    float  lensFlareIntensity;
    int    lensFlareGhostCount;
    float  lensFlareHaloWidth;
    float  lensFlareDistort;

    // PCSS (Percentage Closer Soft Shadows)
    float  pcssLightRadius;     // ライト半径 (world space) — 大きいほどソフト
    int    pcssEnabled;         // 0=PCF, 1=PCSS
    float  _pcssPad0;
    float  _pcssPad1;

    // LUT Color Grading
    float  lutBlend;            // LUT とオリジナル色のブレンド比
    // 天候 (WeatherComponent)。LUT ブロックの空き 3 枠を充てるため cbuffer レイアウトは変わらない。
    // WHY: 新しい CB を足すと b0〜b13 の枠と全パスの DrawCall 束縛を触ることになり、
    //      「濡らす」ためだけの変更としては波及が大きすぎる。skyDimmer が _lightPad2 を
    //      使っているのと同じ扱い。
    float  weatherWetness;      // 濡れ量 [0,1]。0 で完全に素通りする
    float  weatherDarkening;    // 濡れによる albedo 減衰 [0,1]
    float  weatherPuddle;       // 水平面に溜まる量 [0,1]

    // Reprojection 行列 (TAA / Motion Blur 共用)
    float4x4 prevViewProjection;
    float4x4 invPrevViewProjection;

    // Volumetric Lighting (拡張分)
    float  volMinDist;          // 積分を始める距離
    float  volDensity;          // 大気の消散係数 (0 で減衰なし)
    float  volHeightFalloff;    // 高度による密度減衰 [1/m] (0 で無効)
    float  volHeightStart;      // 減衰の基準高度 [world Y]
    float3 volTint;             // 光芒に掛ける色
    float  volEdgeFade;         // volMaxDist 手前のフェード幅 (割合)

    // 自動露出 (眼の順応)。Composite だけが読む。
    // autoExposureKey <= 0 のとき自動露出は無効で、b5 の exposure がそのまま使われる。
    float  autoExposureKey;          // 平均輝度を持っていく中間グレー (既定 0.18)
    float  autoExposureCompensation; // 手動オフセット [EV]
    float  autoExposureMinEV;        // 露出の下限 [EV]
    float  autoExposureMaxEV;        // 露出の上限 [EV]

    // Forward のマテリアルが画面空間 AO / 接触影をどれだけ受けるか。0 で引かない。
    // WHY Deferred では 0 か: あちらは DeferredLighting が同じ結果を適用済みで、
    //     マテリアル側でも引くと二重に暗くなる。適用箇所は必ずどちらか一方。
    float  screenAoStrength;
    float  screenContactShadowStrength;
    // バッファ解像度 / 描画解像度。AO と接触影は半解像度で焼かれるので 0.5 が入る。
    // WHY 必要か: svPosition.xy は描画解像度のピクセル座標。そのまま Load すると
    //     半解像度バッファの左上 1/4 だけを引き伸ばして読むことになる。
    float  screenAoScale;
    float  screenContactShadowScale;
};
// Shadow.hlsli が b8 宣言の有無を判定するためのガード。
// WHY: Terrain 等 b8 を宣言しないシェーダーでは pcssEnabled/pcssLightRadius が
//      未定義になりコンパイルエラーになる。本 define でフォールバックを切り替える。
#define HAVE_ADVANCED_GRAPHICS_CB 1

#endif // ADVANCED_GRAPHICS_CONSTANTS_HLSLI
