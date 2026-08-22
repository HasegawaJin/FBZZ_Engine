// FBZZ Engine
// RenderPassContext.hpp | fbzz::scene
// RenderGraph 注入パスと各描画パスが共有する実行コンテキスト
#pragma once

#include <Engine/Renderer/Camera.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/LightSystem.hpp>
#include <Engine/Renderer/RenderGraph.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Scene/CameraCullingSettings.hpp>
#include <Engine/Scene/Systems/RenderPasses/EnvironmentResources.hpp>
#include <Engine/Scene/Systems/RenderPasses/OcclusionCuller.hpp>
#include <Math/Frustum.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Physics/Layer.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fbzz::physics { class World; }

namespace fbzz::scene {

class Scene;
struct RenderPassContext;

// UserRenderPassInjectionPoint — Script が追加するパスを既存パイプラインのどこへ挿入するかを表す。
// WHY: RenderGraph は依存関係で実行順を決めるが、HDR へ ReadWrite する透明系パスは同じ依存を持ちやすい。
//      明示的な挿入点を持たせ、Water / VFX / PostProcess 前処理の意図をコードから読めるようにする。
enum class UserRenderPassInjectionPoint : uint8_t {
    AfterOpaque,
    AfterTransparent,
    BeforePostProcess
};

// UserRenderPassDesc — Script / Scene が RenderGraph へ追加したい 1 パス分の宣言。
// WHAT: reads / writes は RenderGraph 上の論理リソース名、execute は実際の描画処理を受け持つ。
//       execute は RenderSystem が保持する RenderPassContext を渡して呼ぶため、Script 側は renderer/resources/handles を参照できる。
struct UserRenderPassDesc {
    std::string name;
    UserRenderPassInjectionPoint injectionPoint = UserRenderPassInjectionPoint::AfterTransparent;
    std::vector<renderer::RenderGraph::ResourceAccess> accesses;
    std::function<void(RenderPassContext&)> execute;
    bool allowCulling = true;
};

// GPU パーティクル CS が参照する力場 1 本分 (48 bytes)。
// LAYOUT: ParticleGpuSim.cs.hlsl の GpuForceField と完全に一致させること。
struct GpuForceField {
    math::Vector4 posRadius;   // xyz=ワールド位置, w=影響半径 (<=0 で無限)
    math::Vector4 dirStrength; // xyz=風向き/渦軸 (ワールド・正規化済み), w=強さ
    math::Vector4 params;      // x=種類(ParticleForceFieldType), y=falloffPower,
                               // z=noiseFrequency, w=noiseSpeed
};

// 1 フレームに GPU パーティクルへ渡せる力場の上限。
// WHY: cbuffer は固定長のため上限を切る。超過分は ParticlePass が近い順ではなく
//      シーン順で切り捨てる (力場が 8 本を超えるシーンは想定しない)。
inline constexpr int kMaxGpuForceFields = 8;

// GPU パーティクル CS 用定数バッファ (b0) — 688 bytes, 16-byte aligned
struct GpuParticleEmitterCB {
    math::Vector3 emitterPos;
    float         deltaTime;
    math::Vector3 gravity;
    uint32_t      maxParticles;
    math::Vector4 colorStart;
    math::Vector4 colorEnd;
    uint32_t      spawnCount;       // 今フレームのスポーン数
    uint32_t      spawnOffset;      // リングバッファ書き込み先頭インデックス
    float         colorCurvePower;  // CPU と一致: pow(t, colorCurvePower)
    float         velocityDamping;
    float         sizeStart;
    float         sizeEnd;
    float         sizeCurvePower;
    float         pad0;
    uint32_t      spriteColumns;
    uint32_t      spriteRows;
    uint32_t      spriteStartFrame;
    uint32_t      spriteEndFrame;
    // ── ノイズモジュール + 力場 (末尾追加で既存オフセットを変えない) ──
    float         time;             // カールノイズのスクロールに使う経過時間
    float         noiseStrength;    // エミッター固有乱流の強さ (0 で無効)
    float         noiseFrequency;
    float         noiseSpeed;
    uint32_t      forceFieldCount;  // gForceFields の有効本数
    uint32_t      flipbookMode;
    float         flipbookFramesPerSecond;
    float         pad1;
    GpuForceField forceFields[kMaxGpuForceFields];
    math::Vector4 curveFlags;       // x=size, y=velocity, z=gradient, w=frameBlend
    math::Vector4 sizeCurveKeys01;  // time0,value0,time1,value1
    math::Vector4 sizeCurveKeys23;
    math::Vector4 velocityCurveKeys01;
    math::Vector4 velocityCurveKeys23;
    math::Vector4 gradientTimes;
    math::Vector4 gradientColors[4];
    math::Matrix4 viewProjection;
    float         screenWidth;
    float         screenHeight;
    float         depthThickness;
    float         depthBounciness;
    uint32_t      depthCollision;
    uint32_t      depthResponse;
    float         depthDamping;
    float         depthPad;
    // ── over-lifetime モジュール追加分 (末尾追加で既存オフセットを変えない) ──
    // 既存 curveFlags が埋まっているため 2 本目のフラグ束を持つ。
    math::Vector4 curveFlags2;          // x=rotation, y=drag, z/w=予約
    math::Vector4 rotationCurveKeys01;  // time0,value0,time1,value1
    math::Vector4 rotationCurveKeys23;
    math::Vector4 dragCurveKeys01;
    math::Vector4 dragCurveKeys23;
    math::Vector3 orbitalAxis;          // 正規化済み
    float         orbitalVelocity;
    float         radialVelocity;
    // bit0 = spriteRandomStartFrame / bit1 = spriteRandomRow
    uint32_t      spriteRandomFlags;
    float         velocityPad0;
    float         velocityPad1;
    // ── カーブ 8 キー化の追加分 ──────────────────────────────────────────────
    // WHY: 既存の *Keys01/23 (キー 0〜3) はオフセットを変えずそのまま残し、
    //      キー 4〜7 を末尾へ足す。CB は「末尾追加のみ」を規約にしてあり、
    //      途中へ挿すと HLSL 側の全オフセットがずれて静かに壊れる。
    math::Vector4 sizeCurveKeys45;
    math::Vector4 sizeCurveKeys67;
    math::Vector4 velocityCurveKeys45;
    math::Vector4 velocityCurveKeys67;
    math::Vector4 rotationCurveKeys45;
    math::Vector4 rotationCurveKeys67;
    math::Vector4 dragCurveKeys45;
    math::Vector4 dragCurveKeys67;
    math::Vector4 gradientTimes47;
    math::Vector4 gradientColors47[4];
    // 実キー数。4 キー固定だった頃は不要だったが、8 キー化で「どこまでが有効か」を
    // GPU 側も知らないと、末尾のダミーキーを踏んで CPU と違う値を返す。
    math::Vector4 curveKeyCounts;   // x=size, y=velocity, z=rotation, w=drag
    // 補間モード (0=Linear, 1=Step, 2=Smooth)。CPU の ApplyCurveInterpolation と対。
    math::Vector4 curveModes;       // x=size, y=velocity, z=rotation, w=drag
    math::Vector4 gradientMeta;     // x=キー数, y=補間モード, z/w=予約
};
// 内訳: 従来 784 + over-lifetime 追加分 112
//   (float4 × 5 = 80) + (orbitalAxis 12 + orbitalVelocity 4 = 16)
//   + (radialVelocity 4 + spriteRandomFlags 4 + pad 4 × 2 = 16)
// + カーブ 8 キー化 256 (float4 × 16: curve 4 本 × 2 + gradient 1 + 色 4 + meta 3)
static_assert(sizeof(GpuParticleEmitterCB) == 1152,
    "GpuParticleEmitterCB must match GpuEmitterCB in ParticleGpuSim.cs.hlsl (1152 bytes)");

struct PerFrameCB {
    math::Matrix4 view;
    math::Matrix4 projection;
    math::Matrix4 viewProjection;
    math::Matrix4 invViewProjection;
    math::Vector3 cameraPos;
    float         nearZ;
    float         farZ;
    float         _pad[3];
};

/// TAA サブピクセルジッターを織り込んだ射影行列を返す。
/// @param jitterNdcX,jitterNdcY NDC 単位のジッター量。TAA 非有効時は 0 を渡す。
/// @note ジッターはラスタライズする行列にだけ乗せる。カリング用の錐台には載せないこと
///       (半ピクセルのために可視判定を揺らす意味がない)。
inline math::Matrix4 MakeJitteredProjection(const renderer::Camera& camera,
                                            float jitterNdcX, float jitterNdcY)
{
    math::Matrix4 projection = camera.GetProjectionMatrix();
    // 列ベクトル規約 (clip = P * viewPos) なので、m[0][2] / m[1][2] に足すと
    // clip.xy += jitter * clip.w となり、深度に依らない一定のピクセルずれになる。
    projection.m[0][2] += jitterNdcX;
    projection.m[1][2] += jitterNdcY;
    return projection;
}

/// ジッター込みの ViewProjection。b0 を使わず自前で行列を組むパス用。
inline math::Matrix4 MakeJitteredViewProjection(const renderer::Camera& camera,
                                                float jitterNdcX, float jitterNdcY)
{
    return MakeJitteredProjection(camera, jitterNdcX, jitterNdcY) * camera.GetViewMatrix();
}

/// カメラと TAA サブピクセルジッターから b0 (PerFrameCB) を組む。
/// @note invViewProjection をジッター込みの viewProjection から作るのは、深度バッファを
///       焼いた射影と揃えないと復元したワールド座標がずれるため。逆に再投影先の
///       prevViewProjection 側はジッターを載せない — 履歴バッファはピクセル中心で
///       収束した絵なので、そこへはピクセル中心座標で引く必要がある。
inline PerFrameCB MakeCameraFrameCB(const renderer::Camera& camera,
                                    float jitterNdcX, float jitterNdcY)
{
    const math::Matrix4 projection = MakeJitteredProjection(camera, jitterNdcX, jitterNdcY);

    PerFrameCB frameData{};
    frameData.view              = camera.GetViewMatrix();
    frameData.projection        = projection;
    frameData.viewProjection    = projection * frameData.view;
    frameData.invViewProjection = math::Matrix4::Inverse(frameData.viewProjection);
    frameData.cameraPos         = camera.m_position;
    frameData.nearZ             = camera.m_near;
    frameData.farZ              = camera.m_far;
    return frameData;
}

struct PerObjectCB {
    math::Matrix4 world;
    math::Matrix4 worldInvTranspose;
};

// ── クラスタライトカリング (Forward+ / Deferred+) ────────────────────────────
// LAYOUT: Assets/Shaders/Common/ClusterConstants.hlsli と完全に一致させること。
//         片方だけ変えるとライトが黙って別の位置・別の色で評価される。

// グリッドは解像度非依存の固定分割。
// WHY: 画面ピクセル数からタイル数を決めると、ビューポートをリサイズするたびに
//      クラスタバッファを作り直すことになる。16:9 に合わせた固定分割なら確保は起動時 1 回で済む。
inline constexpr uint32_t kClusterGridX = 32;
inline constexpr uint32_t kClusterGridY = 18;
inline constexpr uint32_t kClusterGridZ = 24;
inline constexpr uint32_t kClusterCount = kClusterGridX * kClusterGridY * kClusterGridZ;

// 1 クラスタが保持できるライト数。あふれた分はライト番号の昇順で切り捨てる (決定的)。
inline constexpr uint32_t kMaxLightsPerCluster = 32;
// クラスタ 1 個分の uint 数。先頭がライト数、続けてライト番号が並ぶ。
inline constexpr uint32_t kClusterStride = kMaxLightsPerCluster + 1;
// 点光源 + スポットを統合した配列の上限 (従来は点 8 / スポット 4 だった)。
inline constexpr uint32_t kMaxPunctualLights = 256;

// ライト供給モード。ClusterConstants.hlsli の FBZZ_LIGHT_MODE_* と一致させること。
// WHY 3 状態か: cbuffer 未束縛のパスは中身が全ゼロで読まれる。0 = Legacy にしておけば、
//      b9 と t29/t30 を渡していない既存パスは今までと 1 ビットも変わらず動く。
enum class ClusterLightMode : uint32_t {
    Legacy    = 0, // b3 の固定長 cbuffer (点 8 / スポット 4)
    Linear    = 1, // StructuredBuffer を全数走査 (カリング無効・A/B 検証用)
    Clustered = 2, // クラスタが持つライトだけ走査
};

// 点光源とスポットを統合した 1 本ぶん (64 bytes)。
// WHY 統合するか: インデックス空間が 1 本になり、カリング CS も PS も 1 重ループで済む。
struct PunctualLightGPU {
    math::Vector3 position;  float    range;
    math::Vector3 color;     float    intensity;
    math::Vector3 direction; float    innerCos;   // Spot のみ
    float         outerCos;  uint32_t type;       float _lightPad[2];
};
static_assert(sizeof(PunctualLightGPU) == 64,
    "PunctualLightGPU must match PunctualLight in Common/ClusterConstants.hlsli (64 bytes)");

struct ClusterConstantsCB {
    // 1 クラスタが覆う画面上のピクセル数 = screenSize / (GridX, GridY)。
    // WHY CPU 側で割るか: マテリアルパスは PostProcConstants (b5) を束縛しないため、
    //     シェーダー側で screenSize を参照できない。
    float    clusterTilePx[2];
    float    clusterSliceScale;
    float    clusterSliceBias;
    uint32_t clusterLightMode;   // ClusterLightMode
    uint32_t punctualLightCount;
    uint32_t clusterDebugMode;   // 0=通常, 1=クラスタあたりライト数のヒートマップ
    uint32_t _clusterPad0 = 0;
};
static_assert(sizeof(ClusterConstantsCB) == 32,
    "ClusterConstantsCB must match ClusterConstants in Common/ClusterConstants.hlsli (32 bytes)");

// ShadowConstantsCB — HLSL の ShadowConstants (b4) と 1 対 1 で対応する。
// LAYOUT: Assets/Shaders/Common/ShadowConstants.hlsli と完全に一致させること。
//         あちらが唯一の HLSL 側定義 (Constants / Terrain / Water が include する)。
struct ShadowConstantsCB {
    // 単一のライト行列で足りるパス向け (= cascadeViewProjection[0] と同じ内容)。
    // パーティクル自己影・体積光など、カスケードの概念を持たない経路が使う。
    math::Matrix4 lightViewProjection;
    // カスケードごとのライト viewProjection。有効なのは先頭 cascadeCount 本。
    math::Matrix4 cascadeViewProjection[renderer::kMaxShadowCascades];
    // カスケードごとのアトラス矩形。xy = UV オフセット, zw = UV スケール。
    math::Vector4 cascadeAtlasRect[renderer::kMaxShadowCascades];
    // カスケードごとの NDC 深度バイアス (x=cascade0 .. w=cascade3)。
    // WHY: カスケードごとに正射影の深度レンジが違うため、同じワールド距離のオフセットでも
    //      NDC 換算値が変わる。1 つの値を共有するとどこかで必ず破綻する。
    math::Vector4 cascadeBias;

    float         shadowMapTexelSize[2]; // 1.0 / アトラス全体の解像度
    float         shadowBias;            // 単一カスケード時のバイアス (= cascadeBias.x)
    float         shadowStrength;        // 0=影なし, 1=完全な影

    int           shadowPcfRadius;  // PCF カーネル半径: 0=ハード, 1=3x3, 2=5x5, 3=7x7
    int           cascadeCount;     // 1 = 単一シャドウマップ (従来), 2〜4 = CSM
    float         cascadeBlend;     // カスケード境界のクロスフェード幅 [0,1]
    int           cascadeDebugView; // 1 = カスケード番号を色で可視化

    // 雲シャドウ (Phase C)
    float         cloudShadowStrength; // 0=無効
    float         cloudShadowCoverage;
    float         cloudShadowScale;
    float         cloudShadowSpeed;

    float         cloudShadowTime;
    float         cloudShadowWindX;
    float         cloudShadowWindZ;
    float         _shadowPad0 = 0.0f;
};
static_assert(sizeof(ShadowConstantsCB) == 464,
    "ShadowConstantsCB must match ShadowConstants in Common/ShadowConstants.hlsli (464 bytes)");

struct AtmosphereCB {
    float rayleighScattering[3];
    float mieScattering;
    float planetRadius;
    float atmosphereRadius;
    float sunIntensity;
    float mieG;
    // ── 月 (Phase B) ── HLSL AtmosphereConstants と一致させること (末尾追加・16byte 整列)。
    float moonEnabled;     // 0/1
    float moonSize;
    float moonBrightness;
    float _moonPad0;
    float moonColor[3];
    float _moonPad1;
};

// AdvancedGraphicsCB — IBL・SSR・TAA・GTAO・Contact Shadow 等の詳細設定。
// LAYOUT: Constants.hlsli の AdvancedGraphicsConstants cbuffer と完全に一致させること。
// WHY: 16-byte アライメント制約のため、各グループを 4 要素単位でまとめる。
struct AdvancedGraphicsCB {
    // IBL
    float iblIntensity;       float iblDiffuseScale;    float iblSpecularScale;   int   iblMaxMipLevel;
    // SSR
    float ssrMaxDistance;     float ssrThickness;        int   ssrSteps;           float ssrIntensity;
    // Volumetric
    float volLightIntensity;  float volScattering;       int   volSteps;           float volMaxDist;
    // TAA
    float taaFeedback;        float taaJitterX;          float taaJitterY;         float _taaPad;
    // Motion Blur
    // screenWidth/screenHeight は MotionBlur CS が b5 非バインド下で screenSize の代替として参照する
    float motionBlurStrength; int   motionBlurSamples;   float screenWidth;         float screenHeight;
    // GTAO
    float gtaoIntensity;      float gtaoRadius;          int   gtaoSlices;         int   gtaoStepsPerSlice;
    // Contact Shadows
    float contactShadowStrength; float contactShadowRayLen; int contactShadowSteps; float contactShadowThick;
    // Lens Flare
    float lensFlareIntensity; int   lensFlareGhostCount; float lensFlareHaloWidth; float lensFlareDistort;
    // PCSS
    float pcssLightRadius;    int   pcssEnabled;         float _pcssPad0;          float _pcssPad1;
    // LUT
    float lutBlend;           float _lutPad0;            float _lutPad1;           float _lutPad2;
    // Reprojection 行列 (TAA / Motion Blur 共用)
    math::Matrix4 prevViewProjection;
    math::Matrix4 invPrevViewProjection;
    // Volumetric Lighting (拡張分)
    float volMinDist;         float volDensity;          float volHeightFalloff;   float volHeightStart;
    float volTintR;           float volTintG;            float volTintB;           float volEdgeFade;
};
static_assert(sizeof(AdvancedGraphicsCB) == 320,
    "AdvancedGraphicsCB must match AdvancedGraphicsConstants in Constants.hlsli (320 bytes)");

struct PostProcCB {
    float texelSize[2];
    float screenSize[2];
    float exposure;
    float time;
    float fogDensity;
    float bloomIntensity;
    float fogColor[3];
    float fogFar;
    float contrast;
    float saturation;
    float hueShift;
    float temperature;
    float tint;
    float vignetteIntensity;
    float vignetteSmoothness;
    float vignetteRoundness;
    float vignetteColor[3];
    float filmGrainIntensity;
    float filmGrainResponse;
    float chromaticAberration;
    float lensDistortion;
    float ssaoIntensity;
    float customIntensity;
    float customBlend;
    float _customPad[2];
    float customParameters[4];
    float underwaterStrength;
    float underwaterDepth;
    float _underwaterPad[2];
    float underwaterColor[3];
    float underwaterFogDensity;
    float sharpenStrength;
    float sharpenRadius;
    float dofFocusDistance;
    float dofFocusRange;
    float dofBlurRadius;
    float sepiaIntensity;
    float invertIntensity;
    float posterizeLevels;
    float pixelSize;
    float _stylizedPad[3];
    float bloomThreshold;
    float bloomSoftKnee;
    float clarityStrength;
    float clarityRadius;
    float shadowLift;
    float highlightCompression;
    float colorFilterIntensity;
    float _qualityPad0;
    float colorFilter[3];
    float _qualityPad1;
    // 画面フェード — Composite パスの最終出力に適用する。alpha=0 で通常, 1 で全面フェード色。
    float screenFadeColor[3];
    float screenFadeAlpha;
    // 大気フォグ統合 (環境システム §3-3): フォグ色の出どころ。0=指数(従来), 1=大気散乱(エアリアル)。
    // WHY: 末尾に追加し既存フィールドのオフセットを変えない (HLSL PostProcConstants と一致)。
    float fogSource;
    float _fogPad[3];
    // 投影コースティクス改良 (Phase C-2): 水域 XZ 範囲フェード + 波連動 UV ゆらぎ。
    // HalfExtent<=0 で範囲無制限 (後方互換)。WaveAmp=0 でゆらぎ無し。
    float causticsCenterX;
    float causticsCenterZ;
    float causticsHalfExtentX;
    float causticsHalfExtentZ;
    float causticsWaveAmp;
    float causticsWaveFreq;
    float causticsWaveSpeed;
    float _causticsPad;
};

/// 画面サイズ由来のフィールドだけを埋めた PostProcCB を返す。
/// @note b5 は全ポストプロセスで共有され、各パスが構造体ごと上書きする。texelSize/screenSize を
///       入れ忘れたパスは前パス (あるいは前フレーム) の残りを読んで静かに壊れるので、
///       b5 を使うパスは必ずここを起点にしてから固有フィールドを足すこと。
inline PostProcCB MakeScreenPostProcCB(uint32_t width, uint32_t height)
{
    const float w = static_cast<float>(width  != 0u ? width  : 1u);
    const float h = static_cast<float>(height != 0u ? height : 1u);
    PostProcCB data{};
    data.texelSize[0]  = 1.0f / w;
    data.texelSize[1]  = 1.0f / h;
    data.screenSize[0] = w;
    data.screenSize[1] = h;
    return data;
}

struct OutlineCB {
    math::Vector4 color;
    float         width;
    float         _pad[3];
};

// DetailGrassCB (b2) — DetailGrass.hlsl の DetailGrassCB cbuffer と完全に一致させること。
// 48 bytes, 16-byte aligned
struct DetailGrassCB {
    float    windDir[3];    // 正規化風向き (XZ 平面)
    float    gTime;         // 累積時間
    float    windStrength;  // グローバル風速
    float    windFrequency; // sin 周波数
    float    bladeHeight;   // ブレード高さ [m]
    float    bladeWidth;    // ブレード根元幅 [m]
    int32_t  bladeSegments; // 分割数
    float    alphaCutoff;   // アルファテスト閾値
    int32_t  hasAlbedoTex;  // 1 = テクスチャあり
    float    _pad;
};
static_assert(sizeof(DetailGrassCB) == 48, "DetailGrassCB size mismatch");

// デカールの受信レイヤーフィルタが有効であることを示す DecalCB::flags のビット。
inline constexpr uint32_t kDecalFlagReceiverFilter = 1u;

// DecalConstants (b10) — Material/Decal/DecalCommon.hlsli と完全に一致させること。
//
// WHY b2 ではないか: b2 は cbuffer 名 "MaterialConstants" として .mat の
//   リフレクション対象になる。デカールの投影データをそこへ置くと、デカールだけ
//   マテリアルを持てない例外になる (Assets/Shaders/Common/Binding.hlsli 参照)。
struct DecalCB {
    math::Matrix4 invDecalWorld;
    math::Vector3 decalTangent;
    float         _pad0 = 0.0f;
    math::Vector3 decalBitangent;
    float         _pad1 = 0.0f;
    math::Vector3 decalNormal;
    float         _pad2 = 0.0f;
    float         alpha = 1.0f;
    // 角度フェード。受け面の法線が投影軸から傾くほどデカールを薄くする。
    // WHY: OBB 投影は投影軸に対して斜めな面へ当てると、テクスチャが引き伸ばされて
    //      長い筋になる。着弾痕や血痕が壁の角をまたいだ瞬間に「伸びた汚れ」として
    //      露見する、デカールで最も目立つ破綻がこれ。
    //      角度で薄めれば、破綻する範囲がそのまま消える。
    float         angleFadeStrength = 1.0f; // 0 = フェードなし
    float         angleFadeCos      = 0.34f; // この cos より寝た面では完全に消える (既定 70 度)
    uint32_t      flags             = 0;
    uint32_t      receiverLayerMask = ~0u;
    float         _pad3[3] = { 0.0f, 0.0f, 0.0f };
};
static_assert(sizeof(DecalCB) == 144, "DecalCB size mismatch");

// 組み込み Decal.hlsl の MaterialConstants (b2)。
// .mat を割り当てていない DecalComponent へは DecalPass がこの形で値を流す。
// LAYOUT: Assets/Shaders/Material/Decal/Decal.hlsl と完全に一致させること。
struct DecalMaterialCB {
    float    albedoTint[4]    = { 1.0f, 1.0f, 1.0f, 1.0f };
    float    emissiveColor[3] = { 1.0f, 1.0f, 1.0f };
    float    emissiveScale    = 0.0f;
    float    uvTiling[2]      = { 1.0f, 1.0f };
    float    uvOffset[2]      = { 0.0f, 0.0f };
    float    normalStrength   = 1.0f;
    // Material::Upload と同じ規則: bit i = テクスチャスロット i が有効。
    // bit0=albedo(t0) bit1=normal(t1) bit3=emissive(t3)
    uint32_t textureMask      = 0;
    float    _pad[2]          = { 0.0f, 0.0f };
};
static_assert(sizeof(DecalMaterialCB) == 64, "DecalMaterialCB size mismatch");

// 受信レイヤーバッファを描くドローの b10。DecalMask(.Skinned).hlsl と一致させること。
struct DecalReceiverCB {
    // レイヤー番号 + 1。0 はクリア値 (未描画) と衝突するため使わない。
    float layerEncoded = 0.0f;
    float _pad[3]      = { 0.0f, 0.0f, 0.0f };
};
static_assert(sizeof(DecalReceiverCB) == 16, "DecalReceiverCB size mismatch");

struct RenderPassHandles {
    renderer::ResourceHandle<renderer::RenderTargetTag> shadowMapRT;
    renderer::ResourceHandle<renderer::RenderTargetTag> hdrRT;
    renderer::ResourceHandle<renderer::RenderTargetTag> ldrRT;
    renderer::ResourceHandle<renderer::RenderTargetTag> selectionMaskRT;
    renderer::ResourceHandle<renderer::RenderTargetTag> outlineRT;
    renderer::ResourceHandle<renderer::RenderTargetTag> customPostProcessRT[2];
    renderer::ResourceHandle<renderer::RenderTargetTag> gbufferRT;

    renderer::ResourceHandle<renderer::TextureTag> bloomHalf;
    renderer::ResourceHandle<renderer::TextureTag> bloomFull;
    renderer::ResourceHandle<renderer::TextureTag> ssaoRaw;
    renderer::ResourceHandle<renderer::TextureTag> ssaoBlur;
    renderer::ResourceHandle<renderer::TextureTag> shadowDepthTex;
    renderer::ResourceHandle<renderer::TextureTag> fxaaInput;
    renderer::ResourceHandle<renderer::TextureTag> postProcessInput;

    renderer::ResourceHandle<renderer::ShaderTag> bloomDownShader;
    renderer::ResourceHandle<renderer::ShaderTag> bloomUpShader;
    renderer::ResourceHandle<renderer::ShaderTag> ssaoShader;
    renderer::ResourceHandle<renderer::ShaderTag> ssaoBlurShader;
    renderer::ResourceHandle<renderer::ShaderTag> compositeShader;
    renderer::ResourceHandle<renderer::ShaderTag> causticsShader;
    renderer::ResourceHandle<renderer::ShaderTag> selectionMaskShader;
    renderer::ResourceHandle<renderer::ShaderTag> selectionMaskSkinnedShader;
    renderer::ResourceHandle<renderer::ShaderTag> selectionMaskParticleShader;
    renderer::ResourceHandle<renderer::ShaderTag> selectionMaskParticleGpuShader;
    renderer::ResourceHandle<renderer::ShaderTag> selectionOutlineShader;
    renderer::ResourceHandle<renderer::ShaderTag> fxaaShader;
    std::vector<renderer::ResourceHandle<renderer::ShaderTag>> customPostProcessShaders;

    renderer::ResourceHandle<renderer::PipelineStateTag> selectionMaskPSO;
    renderer::ResourceHandle<renderer::PipelineStateTag> postprocPSO;
    renderer::ResourceHandle<renderer::PipelineStateTag> causticsPSO;

    renderer::ResourceHandle<renderer::ConstantBufferTag> frameCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> objectCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> lightCB;
    // 最終フォールバックの単位行列パレット。スケルトンが解決できない場合のみ使う。
    // 通常は Model::referencePoseCB (リファレンスポーズ) が優先される。
    // ResolveSkinningCB() を必ず経由すること。
    renderer::ResourceHandle<renderer::ConstantBufferTag> bindPoseSkinningCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> postprocCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> outlineCB;

    renderer::ResourceHandle<renderer::RenderTargetTag>   decalDepthRT;
    // 可視サーフェスのレイヤー番号 + 1 を持つ受信バッファ。レイヤーフィルタを持つ
    // デカールが 1 つでもある フレームだけ描く。
    renderer::ResourceHandle<renderer::RenderTargetTag>   decalMaskRT;
    renderer::ResourceHandle<renderer::ShaderTag>         decalShader;
    renderer::ResourceHandle<renderer::ShaderTag>         decalMaskShader;
    renderer::ResourceHandle<renderer::ShaderTag>         decalMaskSkinnedShader;
    renderer::ResourceHandle<renderer::PipelineStateTag>  decalPSO;
    renderer::ResourceHandle<renderer::PipelineStateTag>  decalMaskPSO;
    renderer::ResourceHandle<renderer::ConstantBufferTag> decalCB;
    // 組み込みシェーダー用の b2。.mat を持つデカールは Material 側の cbuffer を使う。
    renderer::ResourceHandle<renderer::ConstantBufferTag> decalMaterialCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> decalReceiverCB;

    renderer::ResourceHandle<renderer::ShaderTag>         shadowShader;
    renderer::ResourceHandle<renderer::ShaderTag>         shadowSkinnedShader;
    renderer::ResourceHandle<renderer::ConstantBufferTag> shadowCB;

    // コンピュートスキニング — ボーン変形を 1 フレーム 1 回だけ計算して各パスで共有する。
    renderer::ResourceHandle<renderer::ShaderTag>         skinningComputeCS;
    renderer::ResourceHandle<renderer::ConstantBufferTag> skinningCB;

    renderer::ResourceHandle<renderer::PipelineStateTag>  defaultPSO;
    renderer::ResourceHandle<renderer::PipelineStateTag>  wireframePSO;

    renderer::ResourceHandle<renderer::ShaderTag>         skyShader;
    renderer::ResourceHandle<renderer::ShaderTag>         sunMoonShader;
    renderer::ResourceHandle<renderer::PipelineStateTag>  skyPSO;
    renderer::ResourceHandle<renderer::PipelineStateTag>  sunMoonPSO;
    renderer::ResourceHandle<renderer::BufferTag>         skyVB;
    renderer::ResourceHandle<renderer::BufferTag>         skyIB;
    uint32_t                                              skyIndexCount = 0;
    renderer::ResourceHandle<renderer::ConstantBufferTag> atmosphereCB;

    // 空連動 IBL (環境システム Phase A): SkyCapture の描画先キューブマップと、
    // 6 面それぞれの view/projection を渡す b0 用 CB (カメラ frameCB とは別に持つ)。
    renderer::ResourceHandle<renderer::RenderTargetTag>   skyEnvCubeRT;
    renderer::ResourceHandle<renderer::ConstantBufferTag> skyCaptureFrameCB;

    renderer::ResourceHandle<renderer::ShaderTag>         particleShader;
    renderer::ResourceHandle<renderer::PipelineStateTag>  particlePSO;
    renderer::ResourceHandle<renderer::PipelineStateTag>  particleAlphaPSO;
    renderer::ResourceHandle<renderer::PipelineStateTag>  particlePremultipliedPSO;
    renderer::ResourceHandle<renderer::BufferTag>         particleVB;
    renderer::ResourceHandle<renderer::BufferTag>         particleIB;

    // GPU パーティクル
    renderer::ResourceHandle<renderer::ShaderTag>         particleGpuSimCS;   // CS: シミュレーション+スポーン
    // GPU ソート 3 段。半透明を大量に出すとき、描画順をカメラ距離で並べ替えるために使う。
    // WHY: .cs.hlsl はエントリ 1 本なので、キー生成 / グローバル段 / LDS 段で 3 本に分かれる。
    renderer::ResourceHandle<renderer::ShaderTag>         particleGpuSortKeysCS;
    renderer::ResourceHandle<renderer::ShaderTag>         particleGpuSortStepCS;
    renderer::ResourceHandle<renderer::ShaderTag>         particleGpuSortLocalCS;
    // メッシュパーティクルのインスタンス描画 (VS が SV_InstanceID で粒子を引く)。
    renderer::ResourceHandle<renderer::ShaderTag>         particleGpuMeshShader;
    // 自己影: 光源から見た密度を積む専用 RT / シェーダー / 光源行列を入れた frame CB。
    // WHY: 頂点展開ロジックを Particle.hlsl と共有するため、b0 の view/viewProjection だけを
    //      光源のものへ差し替える。h.frameCB を書き換えると後続パスへ漏れるので別 CB を持つ。
    renderer::ResourceHandle<renderer::RenderTargetTag>   particleSelfShadowRT;
    renderer::ResourceHandle<renderer::ShaderTag>         particleSelfShadowShader;
    renderer::ResourceHandle<renderer::ConstantBufferTag> particleSelfShadowFrameCB;
    // 自己影の密度バッファ 1 辺の解像度 [px]。
    // WHY: 自己影が拾うのは「煙の内部で光がどれだけ減るか」という低周波の情報で、
    //      輪郭の鮮鋭さは要らない。シャドウマップより粗くしてフィルレートを抑える。
    static constexpr std::uint32_t kSelfShadowResolution = 512u;
    renderer::ResourceHandle<renderer::ShaderTag>         particleGpuShader;  // VS+PS: billboard 描画 (加算合成)
    renderer::ResourceHandle<renderer::ShaderTag>         particleGpuAlphaShader; // VS+PS: billboard 描画 (アルファ合成)
    renderer::ResourceHandle<renderer::PipelineStateTag>  particleGpuPSO;
    renderer::ResourceHandle<renderer::PipelineStateTag>  particleGpuAlphaPSO;
    renderer::ResourceHandle<renderer::PipelineStateTag>  particleGpuPremultipliedPSO;

    renderer::ResourceHandle<renderer::ShaderTag>         trailShader;
    renderer::ResourceHandle<renderer::PipelineStateTag>  trailPSO;

    renderer::ResourceHandle<renderer::ShaderTag>         meshTrailShader;
    renderer::ResourceHandle<renderer::ShaderTag>         skinnedMeshTrailShader;
    renderer::ResourceHandle<renderer::PipelineStateTag>  meshTrailPSO;
    renderer::ResourceHandle<renderer::PipelineStateTag>  meshTrailDoubleSidedPSO;

    renderer::ResourceHandle<renderer::ShaderTag>         gbufferShader;
    renderer::ResourceHandle<renderer::ShaderTag>         deferredLightingShader;
    renderer::ResourceHandle<renderer::ShaderTag>         depthCopyShader;

    // ---- クラスタライトカリング (Forward+ / Deferred+) ----
    // punctualLightBuffer は PS の t29 / CS の t14 へ、clusterIndexBuffer は PS の t30 /
    // CS の u2 へ束縛する。clusterCB (b9) はモードとグリッド係数を運ぶ。
    renderer::ResourceHandle<renderer::StructuredBufferTag> punctualLightBuffer;
    renderer::ResourceHandle<renderer::StructuredBufferTag> clusterIndexBuffer;
    renderer::ResourceHandle<renderer::ConstantBufferTag>   clusterCB;
    renderer::ResourceHandle<renderer::ShaderTag>           clusterCullCS;

    // Detail System (Terrain Detail — GPU Instancing)
    renderer::ResourceHandle<renderer::ShaderTag>         detailMeshShader;
    renderer::ResourceHandle<renderer::ShaderTag>         detailBillboardShader;
    renderer::ResourceHandle<renderer::ShaderTag>         detailGrassShader;
    // Deferred 用: GBuffer(MRT) へ書き出す変種（AO/接触影/SSR/PBR を地形・メッシュと同様に適用）。
    renderer::ResourceHandle<renderer::ShaderTag>         detailGBufferShader;
    renderer::ResourceHandle<renderer::ShaderTag>         detailGrassGBufferShader;
    renderer::ResourceHandle<renderer::PipelineStateTag>  detailMeshPSO;    // SOLID + OPAQUE + DEPTH_ON
    renderer::ResourceHandle<renderer::PipelineStateTag>  detailNoCullPSO;  // SOLID_NOCULL + OPAQUE + DEPTH_ON
    renderer::ResourceHandle<renderer::ConstantBufferTag> detailGrassCB;    // b2: DetailGrassCB

    // Foliage System (樹木・大型植生 — SubMesh Material + GPU Instancing)
    renderer::ResourceHandle<renderer::ShaderTag>         foliageShader;
    renderer::ResourceHandle<renderer::ShaderTag>         foliageGBufferShader; // Deferred 用 GBuffer 書き込み変種
    renderer::ResourceHandle<renderer::PipelineStateTag>  foliagePSO;
    renderer::ResourceHandle<renderer::PipelineStateTag>  foliageNoCullPSO;

    // ---- Advanced Graphics ----

    // AdvancedGraphics 共用定数バッファ (b8)
    renderer::ResourceHandle<renderer::ConstantBufferTag> advancedGraphicsCB;

    // IBL (Image-Based Lighting)
    // WHY: テクスチャハンドルは ResourceManager から取得した静的リソース。
    //      シーンのスカイドームが変わるまで再ロード不要。
    renderer::ResourceHandle<renderer::TextureTag>        iblIrradiance;   // Diffuse irradiance cubemap
    renderer::ResourceHandle<renderer::TextureTag>        iblPrefilter;    // Specular prefiltered cubemap
    renderer::ResourceHandle<renderer::TextureTag>        iblBrdfLut;      // BRDF 積分 LUT (512x512 R16G16F)
    renderer::ResourceHandle<renderer::ShaderTag>         iblBrdfBakeShader; // CS: BRDF LUT をスタートアップ時に焼く
    renderer::ResourceHandle<renderer::RenderTargetTag>   iblBrdfLutRT;    // BRDF LUT bake 用 RT (静的)

    // SSR (Screen Space Reflections)
    renderer::ResourceHandle<renderer::TextureTag>        ssrResult;       // SSR Compute 出力テクスチャ
    renderer::ResourceHandle<renderer::ShaderTag>         ssrShader;       // CS

    // Volumetric Lighting
    renderer::ResourceHandle<renderer::TextureTag>        volumetricResult;
    renderer::ResourceHandle<renderer::ShaderTag>         volumetricShader;
    renderer::ResourceHandle<renderer::ShaderTag>         volumetricCloudShader;
    renderer::ResourceHandle<renderer::ShaderTag>         cloudUpscaleShader; // ハーフ解像度→HDR 合成
    renderer::ResourceHandle<renderer::PipelineStateTag>  volumetricCloudPSO;
    renderer::ResourceHandle<renderer::PipelineStateTag>  volumetricCloudPremultipliedPSO; // 雲の premultiplied 合成専用
    renderer::ResourceHandle<renderer::ConstantBufferTag> volumetricCloudCB;
    // 3D ボリューメトリック雲ノイズ (起動時 CPU 焼き・タイラブル)。
    //   shape  = 128³ 低周波 Perlin-Worley + Worley FBM 帯 (RGBA)
    //   detail = 32³  高周波 Worley FBM (縁の侵食用)
    renderer::ResourceHandle<renderer::TextureTag>        cloudShapeTex;
    renderer::ResourceHandle<renderer::TextureTag>        cloudDetailTex;

    // TAA (Temporal Anti-Aliasing)
    // WHY: taaHistory は前フレームの TAA 出力を保持する永続 RT。
    //      解像度変更時のみ再生成し、毎フレーム ping-pong で入れ替える。
    renderer::ResourceHandle<renderer::RenderTargetTag>   taaHistoryA;     // ping-pong バッファ A
    renderer::ResourceHandle<renderer::RenderTargetTag>   taaHistoryB;     // ping-pong バッファ B
    renderer::ResourceHandle<renderer::ShaderTag>         taaShader;       // VS+PS
    renderer::ResourceHandle<renderer::PipelineStateTag>  taaPSO;
    bool                                                  taaFlip = false; // A→B→A... の ping-pong フラグ

    // Motion Blur
    renderer::ResourceHandle<renderer::TextureTag>        motionBlurResult;
    renderer::ResourceHandle<renderer::ShaderTag>         motionBlurShader;

    // GTAO (Ground Truth Ambient Occlusion)
    renderer::ResourceHandle<renderer::TextureTag>        gtaoRaw;
    renderer::ResourceHandle<renderer::TextureTag>        gtaoBlur;
    renderer::ResourceHandle<renderer::ShaderTag>         gtaoShader;
    renderer::ResourceHandle<renderer::ShaderTag>         gtaoBlurShader;

    // Contact Shadows
    renderer::ResourceHandle<renderer::TextureTag>        contactShadowResult;
    renderer::ResourceHandle<renderer::ShaderTag>         contactShadowShader;

    // Lens Flare
    renderer::ResourceHandle<renderer::ShaderTag>         lensFlareShader;
    renderer::ResourceHandle<renderer::PipelineStateTag>  lensFlarePSO;    // ADDITIVE ブレンド
    // CPU生成32^3 RGBA8 LUT。外部DDSに依存せずCompositeのTexture3D(t22)へ束縛する。
    renderer::ResourceHandle<renderer::TextureTag>        proceduralColorLut;
};

// ShadowCascade — カスケード 1 枚ぶんの描画情報。RenderSystem が毎フレーム組み立て、
// ShadowPass がアトラスのタイルへ描き、各ライティングパスが CB へ転送する。
// WHY: カスケードは「視錐台のどの距離帯を担当するか」以外は単一シャドウマップと同じ構造を
//      持つ。行列・カリング錐台・書き込み先タイル・バイアスをひとまとめにしておけば、
//      ShadowPass 側は「タイルを選んで既存の caster 提出を回す」だけで済む。
struct ShadowCascade {
    math::Matrix4 viewProjection;
    // ライトビュー単体。第 3 行がライト前方への射影なので、caster を光源に近い順へ
    // 並べ替えるための深度キー算出に使う (Hi-Z を効かせるための描画順)。
    math::Matrix4 view;
    // ライト視点のワールド位置。
    math::Vector3 eyePos;
    // このカスケードの caster カリング用錐台 (viewProjection から抽出済み)。
    math::Frustum frustum;
    // アトラス上の位置。xy = UV オフセット, zw = UV スケール (HLSL cascadeAtlasRect と同値)。
    math::Vector4 atlasRect;
    // アトラス上のピクセル矩形。ShadowPass が IRenderer::SetViewport へ渡す。
    uint32_t      viewportX    = 0;
    uint32_t      viewportY    = 0;
    uint32_t      viewportSize = 0;
    // このカスケードの正射影深度レンジで正規化した NDC バイアス。
    float         biasNDC = 0.0f;
    // このカスケードの 1 テクセルが覆うワールド距離 [m]。caster の極小カリングに使う。
    float         texelWorldSize = 0.0f;
};

struct RenderPassContext {
    Scene& scene;
    renderer::IRenderer& renderer;
    renderer::ResourceManager& resources;
    const renderer::Camera& camera;
    const renderer::RenderSettings& settings;
    renderer::ResourceHandle<renderer::RenderTargetTag> outputRT;
    fbzz::LayerMask cullingMask;

    // NOTE: ここまでが集成体初期化で埋める前半。参照メンバー handles より前に
    //       既定値付きフィールドを挿すと呼び出し側の初期化子が 1 つずつずれるため、
    //       新しい設定は必ず handles より後ろへ追加すること。
    RenderPassHandles& handles;

    // ---- カリング挙動 (CameraComponent 由来) ----
    // WHY パスが直接 CameraComponent を読まないか: Scene View / VFX プレビューのように
    //      「シーンのメインカメラとは別の視点」で描く経路があり、そこでゲームカメラの
    //      設定が効いてしまうと、エディタ上の見え方が編集対象と食い違う。
    bool  frustumCullingEnabled   = true;
    bool  occlusionCullingEnabled = true;
    // 全バウンディング球へ加算するワールド単位の余白 [m]。
    float cullingBoundsPadding    = 0.0f;
    // 距離カリング。値は解決済み (負値なし)。0 は「無効」。
    float cullMaxDistance         = 0.0f;
    float cullLayerDistances[kCullLayerCount] = {};
    // レイヤー別距離が 1 つでも設定されているか。全 0 のときは 32 要素の探索ごと省く。
    bool  hasLayerCullDistances   = false;
    bool  cullDistanceSpherical   = true;
    // 極小オブジェクトカリングのしきい値 (画面高さ比)。0 は無効。
    float smallObjectScreenHeight = 0.0f;
    // 画面高さ比の算出に使う射影スケール = projection.m[1][1] = 1/tan(fovY/2)。
    // WHY 事前計算して持つか: Camera::GetProjectionMatrix() は毎回行列を組み直して返すため、
    //      オブジェクトごとに呼ぶと判定より行列生成の方が高くつく。
    float cullProjScaleY          = 0.0f;
    // 距離計算用のカメラ前方ベクトル (深度距離モードでのみ使う)。同じ理由で事前計算する。
    math::Vector3 cullCameraForward = { 0.0f, 0.0f, 1.0f };

    uint32_t width = 0;
    uint32_t height = 0;
    bool selectionOutlineEnabled = false;

    // TAA サブピクセルジッター (NDC 単位)。TAA が無効なフレームは 0。
    // WHY: TAA はフレームごとにサンプル点をピクセル内でずらして初めてサブピクセル情報が
    //      集まる。ジッターが無いと再投影とブレンドをするだけで、静止カメラでは同じ絵に
    //      収束してアンチエイリアスにならない。b0 を組む各パスが MakeCameraFrameCB へ渡す。
    float taaJitterNdcX = 0.0f;
    float taaJitterNdcY = 0.0f;

    renderer::LightConstantsCB lightData;

    // ---- クラスタライトカリング ----
    // punctualLights は b3 の固定長配列 (点 8 / スポット 4) と並行して構築される。
    // WHY 併存させるか: b3 は Sky / Terrain / Particle など 20 以上のシェーダーが
    //      directional・ambient を読むために使っており、消すと影響範囲が広すぎる。
    //      点光源とスポットだけをこちらへ逃がし、対応済みのパスから順に切り替える。
    std::vector<PunctualLightGPU> punctualLights;
    ClusterLightMode              clusterLightMode = ClusterLightMode::Legacy;
    bool                          clusterDebugHeatmap = false;

    math::Matrix4               lightVP;
    // 影の光源視点。ビルボードを光源へ正対させる必要があるパス
    // (パーティクル自己影の密度積み) が lightVP の内訳を要求する。
    math::Matrix4               lightView;
    math::Vector3               lightEyePos;
    // GBuffer を使う不透明パイプラインが有効かどうか。
    // WHY: RenderSettings の Forward/Deferred 名ではなく、各パスが GBuffer 入力を読めるかを判定する。
    bool                        isDeferred  = false;
    bool                        ssaoEnabled = false;
    // ライト正射影の深度範囲で正規化済みの NDC バイアス。
    // WHY: near=1, far=shadowRadius*2+40 のため固定 NDC 値はシーンスケール依存になる。
    //      RenderSystem 側で 0.005 / depthRange として渡すことで
    //      ワールド空間で約 5mm 相当の一定バイアスを保つ。
    float                       shadowBiasNDC  = 0.0f;
    float                       shadowStrength = 1.0f;  // LightComponent から流れてくる影の濃さ

    // ── カスケードシャドウ ──────────────────────────────────────────────────
    // 有効なのは先頭 shadowCascadeCount 本。1 のときは従来の単一シャドウマップと等価
    // (カスケード 0 がアトラス全面を占める) なので、パス側に分岐は要らない。
    ShadowCascade               shadowCascades[renderer::kMaxShadowCascades];
    int                         shadowCascadeCount = 1;

    // 雲シャドウ (Phase C) — RenderSystem が SkyRenderer から設定し、影パスが ShadowConstantsCB へ転送する。
    float                       cloudShadowStrength = 0.0f; // 0=無効
    float                       cloudShadowCoverage = 0.5f;
    float                       cloudShadowScale    = 0.02f;
    float                       cloudShadowSpeed    = 1.0f;
    float                       cloudShadowWindX    = 1.0f;
    float                       cloudShadowWindZ    = 0.3f;
    float                       cloudShadowTime     = 0.0f; // RenderSystem が Time::time を設定

    const math::Frustum* cameraFrustum = nullptr;
    // 最遠カスケードの錐台 (= 影が届く範囲全体)。
    // NOTE: ShadowPass はカスケードごとに shadowCascades[i].frustum でカリングする。
    //       こちらは「影の到達範囲に入るか」を 1 回で判定したいパス向けの代表値。
    const math::Frustum* lightFrustum  = nullptr;
    OcclusionCuller*      occlusionCuller = nullptr;
    // 空連動 IBL の永続状態 (フレームをまたぐ。RenderSystem が static 実体を指す)。
    EnvironmentResources* environmentResources = nullptr;
    const physics::World* physicsWorld   = nullptr;

    // カメラ視点で実際に発行した描画の統計。
    // WHY: パスごとに手書きで加算すると新パス追加時に数え漏れる。
    //      ジオメトリ系パスは SubmitCounted() 経由で Submit し、集計を 1 か所に集める。
    int statsTotalObjects    = 0;
    int statsFrustumCulled   = 0;
    int statsOcclusionCulled = 0;
    int statsDistanceCulled    = 0;
    int statsSmallObjectCulled = 0;
    int statsDrawCalls       = 0;
    int statsVertexCount     = 0;
    int statsTriangleCount   = 0;
    // SkinningComputePass がこのフレームのポーズについて実際に処理した仕事量。
    // Scene/Game View が結果を共有した場合も、後側のビューへ同じ値を引き継ぐ。
    uint64_t statsSkinningVertexCount = 0;
    uint32_t statsSkinningDispatchCount = 0;
    // シャドウマップ描画は同じジオメトリを光源視点で再描画するため、
    // カメラ統計に混ぜず独立したカウンターへ集計する。
    int statsShadowDrawCalls     = 0;
    int statsShadowTriangleCount = 0;
    int statsParticleEmitters = 0;
    int statsParticleVisible = 0;
    int statsParticleCulled = 0;
    int statsParticleBudgetDropped = 0;

    // トランジェント RT リゾルバ。RenderPipeline::Execute() が設定する。
    // WHY: パスコールバックが RenderPipeline を直接参照しないよう、
    //      依存方向を逆転させずにハンドル取得を可能にするためのコールバックとして渡す。
    //      DeclareResource で transient=true のリソースのみ有効。
    //      未設定 (nullptr) の場合は空ハンドルを返す。
    std::function<renderer::ResourceHandle<renderer::RenderTargetTag>(std::string_view)> getTransientRT;
};

// DrawCall 1 件が描く三角形数。
// WHY: indexCount=0 の非インデックス描画 (フルスクリーン三角形・SV_VertexID 生成ジオメトリ) は
//      vertexCount を 3 で割る必要があり、加算側で毎回書き分けると数え間違いが起きる。
[[nodiscard]] inline int DrawCallTriangleCount(const renderer::DrawCall& call)
{
    if (call.topology != renderer::PrimitiveTopology::TRIANGLE_LIST) return 0;
    const uint32_t perInstance = call.indexCount > 0 ? call.indexCount / 3u : call.vertexCount / 3u;
    return static_cast<int>(perInstance * (call.instanceCount > 0 ? call.instanceCount : 1u));
}

// DrawCall 1 件が描く頂点数 (インスタンシングを含む)。
// WHY: インデックス描画では「メッシュのユニーク頂点数」を表示したいので vertexCount を優先し、
//      vertexCount を埋めていないパスのために indexCount へフォールバックする。
[[nodiscard]] inline int DrawCallVertexCount(const renderer::DrawCall& call)
{
    const uint32_t perInstance = call.vertexCount > 0 ? call.vertexCount : call.indexCount;
    return static_cast<int>(perInstance * (call.instanceCount > 0 ? call.instanceCount : 1u));
}

// ジオメトリ系パス共通の Submit ラッパー。カメラ視点の描画統計を同時に加算する。
// WHY: Stats パネルの数値は「実際に GPU へ投げた描画」でなければ意味がない。
//      各パスがこのヘルパーを使うことで、パスを増やしても統計が自動的に追従する。
inline void SubmitCounted(RenderPassContext& ctx, const renderer::DrawCall& call)
{
    ctx.renderer.Submit(call, ctx.resources);
    ++ctx.statsDrawCalls;
    ctx.statsVertexCount   += DrawCallVertexCount(call);
    ctx.statsTriangleCount += DrawCallTriangleCount(call);
}

// シャドウマップ用 Submit ラッパー。光源視点の描画をカメラ統計と分けて集計する。
inline void SubmitCountedShadow(RenderPassContext& ctx, const renderer::DrawCall& call)
{
    ctx.renderer.Submit(call, ctx.resources);
    ++ctx.statsShadowDrawCalls;
    ctx.statsShadowTriangleCount += DrawCallTriangleCount(call);
}

// スキンメッシュ描画に使う b3 パレットを解決する。優先順位:
//   1. AnimatorComponent が評価したパレット (アニメーション中)
//   2. Model のリファレンスポーズ (無アニメ時の既定。Unity / Unreal と同じ考え方)
//   3. 単位行列 (スケルトン未解決時のみ。本来は到達しない)
//
// WHY: 以前は 2 が無く、AnimatorComponent が無いだけで単位行列パレットが使われていた。
//   単位行列が正しい姿勢になるのは頂点がモデル空間そのままのアセットに限られ、
//   ノード階層にバインド変換を持つアセット (Blender 由来など) は倒れて描画された。
inline renderer::ResourceHandle<renderer::ConstantBufferTag> ResolveSkinningCB(
    const renderer::ResourceHandle<renderer::ConstantBufferTag>& animatorPalette,
    const asset::Model* model,
    const renderer::ResourceHandle<renderer::ConstantBufferTag>& identityFallback)
{
    if (animatorPalette.IsValid()) return animatorPalette;
    if (model && model->referencePoseCB.IsValid()) return model->referencePoseCB;
    return identityFallback;
}

} // namespace fbzz::scene
