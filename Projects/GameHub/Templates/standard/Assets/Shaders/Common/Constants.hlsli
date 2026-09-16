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
    // Water パスだけがこの枠を waterSsrEnabled として使う。他パスは 0。
    float    _camReserved;
    // 1 = 平行投影。深度バッファと視空間 Z の関係が射影で変わるため、
    // 深度を線形化する側 (Space.hlsli の LinearizeDepth) が式を切り替える。
    float    isOrthographic;
    float    _camPad;
};

// b1 の 2 枠目を別の意味で使いたいシェーダーは、#include より前に
// FBZZ_OBJECT_CONSTANTS を定義して自前で宣言する (MaterialConstants と同じ規約)。
// Velocity パスは法線を扱わないので worldInvTranspose の枠へ prevWorld を入れている。
#ifndef FBZZ_OBJECT_CONSTANTS
cbuffer ObjectConstants : register(CB_OBJECT)
{
    float4x4 world;
    float4x4 worldInvTranspose;
    // x = LOD ディザのしきい値 (Rendering/LodDither.hlsli が解釈する)。
    // WHY 0 が「無効」か: この枠を書かないパスが既定値のまま素通りできるようにするため。
    //     1 を無効値にすると、書き忘れたパスの絵が黙って消える。
    float4   objectParams;
};
#endif // FBZZ_OBJECT_CONSTANTS

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
    // Option の「明るさ」。float3 colorFilter の 4 成分目にあたる空きスロットを流用するため、
    // cbuffer のレイアウトは変わらない。1.0 で無加工。Composite だけが読む。
    float  userBrightness;
    // 画面フェード — Composite パスの最終出力に適用する。
    float3 screenFadeColor;
    float  screenFadeAlpha;
    // 大気フォグ統合 (環境システム §3-3): 0=Exponential(固定 fogColor), 1=Atmosphere(大気散乱)。
    float  fogSource;
    // 放射ブラー (VFXScreenEffect)。画面中心から外へ引き伸ばす量 [0,1]。
    float  radialBlur;
    float2 _fogPad;
    // 投影コースティクス改良 (Phase C-2): 水域 XZ 範囲 + 波連動。C++ PostProcCB と一致。
    float  causticsCenterX;
    float  causticsCenterZ;
    float  causticsHalfExtentX;
    float  causticsHalfExtentZ;
    float  causticsWaveAmp;
    float  causticsWaveFreq;
    float  causticsWaveSpeed;
    float  _causticsPad;
    // Bloom のミップ連鎖 — texelSize は書き込み先、こちらは読み込み元。
    float2 bloomSrcTexel;
    float  bloomApplyThreshold; // 1 = 輝度閾値を掛ける (連鎖の 1 段目だけ)
    float  bloomAdditive;       // 1 = 書き込み先へ加算、0 = 上書き
    // カスタムパスのパラメーター 4〜7 (customParameters が 0〜3)。
    // WHY 末尾か: 途中へ挿すと以降の全オフセットがずれ、この 4 コピーのうち
    //     直し忘れた 1 つだけが静かに別の値を読む。
    float4 customParameters2;
    // カスタムパスの «走り方» (書き手ではなくエンジンが埋める)。
    //   x = 入力 UV のスケール (downscale の逆数) / y = 何回目の反復 / z = 反復の総数
    float4 customPassInfo;
    // 衝撃波リング (VFXScreenEffect)。C++ PostProcCB と一致。Amplitude=0 で無効。
    float2 shockRingCenter;
    float  shockRingRadius;
    float  shockRingWidth;
    float  shockRingAmplitude;
    float3 _shockRingPad;
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

// AdvancedGraphicsConstants (b8) — 定義は Common/AdvancedGraphicsConstants.hlsli が持つ。
// WHY: Terrain / Water も同じ cbuffer を読む必要があり、手書きの部分コピーは
//      末尾へのフィールド追加に追従できなかった。定義は 1 か所に集約する。
#include "Common/AdvancedGraphicsConstants.hlsli"

#define MAX_SKINNING_BONES 128
cbuffer SkinningConstants : register(CB_SKINNING)
{
    float4x4 boneMatrices[MAX_SKINNING_BONES];
};

#endif // CONSTANTS_HLSLI
