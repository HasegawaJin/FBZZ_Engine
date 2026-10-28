/// @file    AdvancedGraphicsConstants.hlsli
/// @brief   AdvancedGraphicsConstants (b8) cbuffer の単一定義。
/// @author  Hasegawa Jin
/// @date    2026-08-25
///
/// @note Constants.hlsli を include できない Terrain / Water (b0 / b1 / b3 が自前レイアウト) も b8 を読むので、定義を 1 か所に集める。
/// @note レイアウトは Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp の AdvancedGraphicsCB と完全に一致させる (464 bytes)。
#ifndef ADVANCED_GRAPHICS_CONSTANTS_HLSLI
#define ADVANCED_GRAPHICS_CONSTANTS_HLSLI

#include "Common/Binding.hlsli"

/// @brief Light Probe Volume 1 つぶんの配置 (48 bytes)。LightProbeGI.hlsli が読む。
/// @note intensity <= 0 でそのボリュームは無効。
struct ProbeVolumeParams
{
    float3 boxMin;       ///< @note 箱の最小角 [world]
    float  intensity;    ///< @note 拡散 GI の倍率
    float3 invSize;      ///< @note 1 / 箱の大きさ [1/m]
    float  fade;         ///< @note 箱の縁で外側へ戻していく幅 [m]
    uint3  grid;         ///< @note 各軸のプローブ数
    float  normalBias;   ///< @note 法線方向へずらして引く距離 [m]
};

/// @brief IBL・SSR・TAA・GTAO・Contact Shadow 等の詳細グラフィクス設定。
/// @note PostProcConstants が埋まっているため新機能は独立 CB に置く。全フィールドは DX11 CB パッキング規則 (16-byte 境界) に従う。
cbuffer AdvancedGraphicsConstants : register(CB_ADVANCED_GRAPHICS)
{
    /// @name IBL
    float  iblIntensity;        ///< @note 全体スケール。0 で IBL 無効
    float  iblDiffuseScale;
    float  iblSpecularScale;
    int    iblMaxMipLevel;      ///< @note prefilter キューブマップの最大 mip

    /// @name SSR
    float  ssrMaxDistance;      ///< @note 最大レイ距離
    float  ssrThickness;        ///< @note 深度交差判定の厚み
    int    ssrSteps;
    float  ssrIntensity;        ///< @note HDR への合成強度

    /// @name Volumetric Lighting
    float  volLightIntensity;   ///< @note 光柱の明るさ
    float  volScattering;       ///< @note Henyey-Greenstein の g
    int    volSteps;
    float  volMaxDist;          ///< @note レイマーチ最大距離

    /// @name TAA
    float  taaFeedback;         ///< @note 前フレームブレンド比 (0=無効, 0.9=標準)
    float  taaJitterX;          ///< @note 現フレームの Halton ジッター
    float  taaJitterY;          ///< @note 現フレームの Halton ジッター
    float  taaPreviousOrthographic;

    /// @name Motion Blur
    /// @note screenWidth/Height は b5 (PostProcConstants) が CS にバインドされないため、MotionBlur CS の境界チェック・UV 計算用にここへ置く。
    float  motionBlurStrength;  ///< @note シャッター角度換算の強度
    int    motionBlurSamples;
    float  screenWidth;         ///< @note レンダーターゲット幅 [px]
    float  screenHeight;        ///< @note レンダーターゲット高さ [px]

    /// @name GTAO
    float  gtaoIntensity;
    float  gtaoRadius;          ///< @note サンプリング半径 (world space)
    int    gtaoSlices;          ///< @note 積分スライス数
    int    gtaoStepsPerSlice;

    /// @name Contact Shadows
    float  contactShadowStrength;
    float  contactShadowRayLen; ///< @note レイ長さ (world space)
    int    contactShadowSteps;
    float  contactShadowThick;  ///< @note 深度比較の厚み

    /// @name Lens Flare
    float  lensFlareIntensity;
    int    lensFlareGhostCount;
    float  lensFlareHaloWidth;
    float  lensFlareDistort;

    /// @name PCSS
    float  pcssLightRadius;     ///< @note ライト半径 (world space)。大きいほどソフト
    int    pcssEnabled;         ///< @note 0=PCF, 1=PCSS
    float  taaPreviousNearZ;
    float  taaPreviousFarZ;

    /// @name LUT Color Grading / 天候
    float  lutBlend;            ///< @note LUT とオリジナル色のブレンド比
    /// @note 天候 (WeatherComponent) は LUT ブロックの空き 3 枠を使う。新 CB を足すと全パスの束縛に波及するため (skyDimmer が _lightPad2 を使うのと同じ扱い)。
    float  weatherWetness;      ///< @note 濡れ量 [0,1]。0 で完全に素通りする
    float  weatherDarkening;    ///< @note 濡れによる albedo 減衰 [0,1]
    float  weatherPuddle;       ///< @note 水平面に溜まる量 [0,1]

    /// @name Reprojection (TAA / Motion Blur 共用)
    float4x4 prevViewProjection;
    float4x4 invPrevViewProjection;

    /// @name Volumetric Lighting (拡張)
    float  volMinDist;          ///< @note 積分を始める距離
    float  volDensity;          ///< @note 大気の消散係数 (0 で減衰なし)
    float  volHeightFalloff;    ///< @note 高度による密度減衰 [1/m] (0 で無効)
    float  volHeightStart;      ///< @note 減衰の基準高度 [world Y]
    float3 volTint;             ///< @note 光芒に掛ける色
    float  volEdgeFade;         ///< @note volMaxDist 手前のフェード幅 (割合)

    /// @name 自動露出
    /// @note Composite だけが読む。autoExposureKey <= 0 で無効になり b5 の exposure がそのまま使われる。
    float  autoExposureKey;          ///< @note 平均輝度を合わせる中間グレー (既定 0.18)
    float  autoExposureCompensation; ///< @note 手動オフセット [EV]
    float  autoExposureMinEV;        ///< @note 露出の下限 [EV]
    float  autoExposureMaxEV;        ///< @note 露出の上限 [EV]

    /// @name Forward の画面空間 AO / 接触影
    /// @note 受ける強さ。0 で引かない。Deferred では DeferredLighting が適用済みなので 0 (両方で引くと二重に暗くなる)。
    float  screenAoStrength;
    float  screenContactShadowStrength;
    /// @note バッファ解像度 / 描画解像度 (半解像度なら 0.5)。svPosition.xy をそのまま Load すると左上 1/4 を引き伸ばして読むため。
    float  screenAoScale;
    float  screenContactShadowScale;

    /// @name Light Probe Volume
    /// @note [0] が内側 (小さい箱、t22)、[1] が外側 (t21)。両方無効なら拡散環境光は IBL キューブだけから来る。
    ProbeVolumeParams probeVolumes[2];
    float  probeSpecularOcclusion; ///< @note プローブの暗さを鏡面 IBL へ移す強さ [0,1]
    uint2 probeVisibilityIndices;
    float _probePad;
};
/// @note Shadow.hlsli が b8 の有無を判定するガード。b8 を宣言しない Terrain 等で pcss* が未定義になるのをフォールバックへ切り替える。
#define HAVE_ADVANCED_GRAPHICS_CB 1

#endif // ADVANCED_GRAPHICS_CONSTANTS_HLSLI
