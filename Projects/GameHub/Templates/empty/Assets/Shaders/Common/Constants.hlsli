// FBZZ Engine
// Constants.hlsli | Common
// Shared constant-buffer declarations for shaders
#ifndef CONSTANTS_HLSLI
#define CONSTANTS_HLSLI

#include "Binding.hlsli"

cbuffer CameraConstants : register(CB_CAMERA)
{
    float4x4 view;
    float4x4 projection;
    float4x4 viewProjection;
    float4x4 invViewProjection;
    float3   cameraPos;
    float    nearZ;
    float    farZ;
    float3   _camPad;
};

cbuffer ObjectConstants : register(CB_OBJECT)
{
    float4x4 world;
    float4x4 worldInvTranspose;
};

// シェーダーごとに独自の MaterialConstants を宣言したい場合は
// #include より前に #define FBZZ_MATERIAL_CONSTANTS を定義する。
// 全パラメータが必要な PBR 系シェーダーはこのデフォルト定義を使う。
#ifndef FBZZ_MATERIAL_CONSTANTS
cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;                // RGBA ベースカラー               offset  0
    float  metallic;              // 金属度 [0, 1]                   offset 16
    float  roughness;             // 粗さ   [0, 1]                   offset 20
    float  normalStrength;        // 法線マップ強度                   offset 24
    float  occlusionStrength;     // AO 強度 [0, 1]                  offset 28
    float3 emissiveColor;         // エミッシブ色 (emissiveScale と乗算) offset 32
    float  emissiveScale;         // エミッシブ強度                   offset 44
    float2 uvTiling;              // UV タイリング (X, Y)             offset 48
    float2 uvOffset;              // UV オフセット (X, Y)             offset 56
    float  alphaCutoff;           // アルファカットオフしきい値         offset 64
    float3 _matPad0;              //                                  offset 68
    uint   textureMask;           // テクスチャ存在フラグ (bit 0-4)    offset 80
    float3 _matPad1;              //                                  offset 84
};
#endif // FBZZ_MATERIAL_CONSTANTS

#define MAX_POINT_LIGHTS 8
#define MAX_SPOT_LIGHTS  4

struct PointLightData
{
    float3 position;
    float  range;
    float3 color;
    float  intensity;
};

struct SpotLightData
{
    float3 position;
    float  range;
    float3 direction;
    float  innerCos;
    float3 color;
    float  outerCos;
    float  intensity;
    float3 _pad;
};

cbuffer LightConstants : register(CB_LIGHT)
{
    float3 lightDir;
    float  _lightPad;
    float3 lightColor;
    float  lightIntensity;
    PointLightData pointLights[MAX_POINT_LIGHTS];
    SpotLightData  spotLights[MAX_SPOT_LIGHTS];
    int   pointLightCount;
    int   spotLightCount;
    // 空・雲・光芒の昼夜減光係数。DirectionalLight の lightIntensity とは独立した軸。
    // WHY: 以前は空系シェーダーが lightIntensity をそのまま減光に使っていたため、
    //      太陽を明るくすると空まで白飛びして両者を別々に詰められなかった。
    //      CB レイアウトを変えずに済むよう、未使用だった _lightPad2 の 1 枠を充てている。
    float  skyDimmer;
    float  _lightPad2;
    float3 ambientColor;
    float  _ambientPad;
};

// ShadowConstants (b4) — カスケード配列を含む定義は Common/ShadowConstants.hlsli が持つ。
// WHY: Terrain / Water も同じ cbuffer を宣言する必要があり、レイアウトを 3 か所へ
//      手書きしていた頃は片方だけ更新した瞬間に静かに壊れた。定義は 1 か所に集約する。
#include "Common/ShadowConstants.hlsli"

cbuffer PostProcConstants : register(CB_POSTPROC)
{
    float2 texelSize;
    float2 screenSize;
    float  exposure;
    float  time;
    float  fogDensity;
    float  bloomIntensity;
    float3 fogColor;
    float  fogFar;
    float  contrast;
    float  saturation;
    float  hueShift;
    float  temperature;
    float  tint;
    float  vignetteIntensity;
    float  vignetteSmoothness;
    float  vignetteRoundness;
    float3 vignetteColor;
    float  filmGrainIntensity;
    float  filmGrainResponse;
    float  chromaticAberration;
    float  lensDistortion;
    float  ssaoIntensity;
    float  customIntensity;
    float  customBlend;
    float2 _customPad;
    float4 customParameters;
    float  underwaterStrength;
    float  underwaterDepth;
    float2 _underwaterPad;
    float3 underwaterColor;
    float  underwaterFogDensity;
    float  sharpenStrength;
    float  sharpenRadius;
    float  dofFocusDistance;
    float  dofFocusRange;
    float  dofBlurRadius;
    float  sepiaIntensity;
    float  invertIntensity;
    float  posterizeLevels;
    float  pixelSize;
    float3 _stylizedPad;
    float  bloomThreshold;
    float  bloomSoftKnee;
    float  clarityStrength;
    float  clarityRadius;
    float  shadowLift;
    float  highlightCompression;
    float  colorFilterIntensity;
    float  _qualityPad0;
    float3 colorFilter;
    float  _qualityPad1;
    // 画面フェード — Composite パスの最終出力に適用する。
    float3 screenFadeColor;
    float  screenFadeAlpha;
    // 大気フォグ統合 (環境システム §3-3): 0=Exponential(固定 fogColor), 1=Atmosphere(大気散乱)。
    float  fogSource;
    float3 _fogPad;
    // 投影コースティクス改良 (Phase C-2): 水域 XZ 範囲 + 波連動。C++ PostProcCB と一致。
    float  causticsCenterX;
    float  causticsCenterZ;
    float  causticsHalfExtentX;
    float  causticsHalfExtentZ;
    float  causticsWaveAmp;
    float  causticsWaveFreq;
    float  causticsWaveSpeed;
    float  _causticsPad;
};

cbuffer AtmosphereConstants : register(CB_ATMOSPHERE)
{
    float3 rayleighScattering;
    float  mieScattering;
    float  planetRadius;
    float  atmosphereRadius;
    float  sunIntensity;
    float  mieG;
    // 月 (Phase B) — C++ AtmosphereCB と一致。
    float  moonEnabled;     // 0/1
    float  moonSize;
    float  moonBrightness;
    float  _moonPad0;
    float3 moonColor;
    float  _moonPad1;
};

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
    float  _lutPad0;
    float  _lutPad1;
    float  _lutPad2;

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
};
// Shadow.hlsli が b8 宣言の有無を判定するためのガード。
// WHY: Terrain 等 b8 を宣言しないシェーダーでは pcssEnabled/pcssLightRadius が
//      未定義になりコンパイルエラーになる。本 define でフォールバックを切り替える。
#define HAVE_ADVANCED_GRAPHICS_CB 1

#define MAX_SKINNING_BONES 128
cbuffer SkinningConstants : register(CB_SKINNING)
{
    float4x4 boneMatrices[MAX_SKINNING_BONES];
};

#endif // CONSTANTS_HLSLI
