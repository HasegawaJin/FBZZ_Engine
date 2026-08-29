/// @file    RenderPassContext.hpp
/// @brief   RenderGraph 注入パスと各描画パスが共有する実行コンテキスト。
/// @author  Hasegawa Jin
/// @date    2026-06-18
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
    // 既存の *Keys01/23 はオフセットを変えずキー 4〜7 を末尾へ足す。
    // CB は「末尾追加のみ」が規約 (途中へ挿すと HLSL 側の全オフセットがずれる)。
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
    // WaterRenderPass だけが waterSsrEnabled として使う枠。他パスは 0 のまま。
    float         _reserved;
    // 平行投影なら 1、遠近投影なら 0。
    // WHY CB に要るか: 深度バッファの値と視空間 Z の関係が射影で変わる。
    //     Space.hlsli の LinearizeDepthAuto がこれで式を切り替えないと、
    //     SSAO / SSR / コンタクトシャドウ / ソフトパーティクル / デカールが
    //     正投影ビューで一斉に破綻する。
    float         isOrthographic;
    float         _pad;
};

/// TAA サブピクセルジッターを織り込んだ射影行列を返す。
/// @param jitterNdcX,jitterNdcY NDC 単位のジッター量。TAA 非有効時は 0 を渡す。
/// @note ジッターはラスタライズする行列にだけ乗せる。カリング用の錐台には載せないこと
///       (半ピクセルのために可視判定を揺らす意味がない)。
inline math::Matrix4 MakeJitteredProjection(const renderer::Camera& camera,
                                            float jitterNdcX, float jitterNdcY)
{
    math::Matrix4 projection = camera.GetProjectionMatrix();
    if (camera.m_projection == renderer::ProjectionMode::Orthographic) {
        // 正投影は clip.w が常に 1 なので、平行移動成分へ直接足す。
        // 透視と同じ m[*][2] へ足すと、ずれ量が視空間 Z に比例してしまう。
        projection.m[0][3] += jitterNdcX;
        projection.m[1][3] += jitterNdcY;
        return projection;
    }
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
    frameData.isOrthographic =
        camera.m_projection == renderer::ProjectionMode::Orthographic ? 1.0f : 0.0f;
    return frameData;
}

struct PerObjectCB {
    math::Matrix4 world;
    math::Matrix4 worldInvTranspose;
    // x = LOD ディザのしきい値。既定 0 は「遷移していない」= 全画素を描く。
    // LAYOUT: Constants.hlsli の ObjectConstants と一致させること。
    math::Vector4 objectParams;
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
// WHY 64 か: 屋内に range が部屋と同程度のライトを数十本置くと 32 では足りず、番号の
//      大きいライトが黙って消える。境界では出入りが点滅に見える。詳細は HLSL 側の comment。
inline constexpr uint32_t kMaxLightsPerCluster = 64;
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

// PunctualLightGPU::type。ClusterConstants.hlsli の FBZZ_LIGHT_TYPE_* と一致させること。
enum class PunctualLightType : uint32_t {
    Point = 0,
    Spot  = 1,
    // 矩形の面光源。position が面の中心、direction が面の法線、tangent / bitangent が
    // 面内の軸で、halfWidth / halfHeight がその半寸法。
    Area  = 2,
    // 球の光源。position が中心、halfWidth が半径。
    Sphere = 3,
    // カプセルの光源 (蛍光灯・ネオン管)。tangent が軸、halfWidth が半径、
    // halfHeight が軸方向の半長。
    Tube   = 4,
};

// 点光源 / スポット / 面光源を統合した 1 本ぶん (96 bytes)。
// 統合するとインデックス空間が 1 本になり、カリング CS も PS も 1 重ループで済む。
// shadowIndex / cookieIndex は punctualShadowVP / lightCookieVP の何番目か。-1 で無効。
// 番号で参照するので、ライト 256 本に対し影は 16 本まで、という配分が CB を太らせない。
struct PunctualLightGPU {
    math::Vector3 position;  float    range;
    math::Vector3 color;     float    intensity;
    math::Vector3 direction; float    innerCos;   // Spot のみ
    float         outerCos;  uint32_t type;       int32_t shadowIndex = -1;
                                                  int32_t cookieIndex = -1;
    math::Vector3 tangent;   float    halfWidth;  // Area のみ
    math::Vector3 bitangent; float    halfHeight; // Area のみ
};
static_assert(sizeof(PunctualLightGPU) == 96,
    "PunctualLightGPU must match PunctualLight in Common/ClusterConstants.hlsli (96 bytes)");

// シャドウを持てる Spot / Point の合計タイル数。Spot は 1 枚、Point はキューブ 6 面。
// LAYOUT: PunctualShadowConstants.hlsli の FBZZ_MAX_PUNCTUAL_SHADOWS と一致させること。
inline constexpr int kMaxPunctualShadows = 16;
// Cookie アトラスのタイル数。FBZZ_MAX_LIGHT_COOKIES と一致させること。
inline constexpr int kMaxLightCookies = 8;
// Cookie アトラスの寸法。4 列 x 2 行 = kMaxLightCookies タイル。
// 8 タイルをちょうど埋める比が 4:2 なので、正方形にすると半分が未使用のまま常駐する。
inline constexpr uint32_t kLightCookieTileSize    = 512u;
inline constexpr uint32_t kLightCookieAtlasCols   = 4u;
inline constexpr uint32_t kLightCookieAtlasRows   = 2u;
inline constexpr uint32_t kLightCookieAtlasWidth  = kLightCookieTileSize * kLightCookieAtlasCols;
inline constexpr uint32_t kLightCookieAtlasHeight = kLightCookieTileSize * kLightCookieAtlasRows;
static_assert(kLightCookieAtlasCols * kLightCookieAtlasRows
                  == static_cast<uint32_t>(kMaxLightCookies),
              "cookie atlas tiling must cover exactly kMaxLightCookies tiles");
// レガシー経路 (b3) が運べるライト数 = 点 8 + スポット 4。
// LAYOUT: Common/Constants.hlsli の MAX_POINT_LIGHTS + MAX_SPOT_LIGHTS と一致させること。
inline constexpr int kMaxLegacyPunctualLights = 12;
// legacyPunctualSlots 内でスポットが始まる位置。
inline constexpr int kLegacySpotSlotBase = 8;
// レガシー経路 (b3) が型を運べない「大きさを持つ光源」(Area / Sphere / Tube) を
// b12 側へ載せる本数。
// LAYOUT: PunctualShadowConstants.hlsli の FBZZ_MAX_LEGACY_SHAPED_LIGHTS と一致させること。
inline constexpr int kMaxLegacyShapedLights = 4;
// 1 本が占める float4 の数。レイアウトは PunctualShadowConstants.hlsli が正本。
//   [0] position/range [1] color/intensity [2] direction/予備
//   [3] tangent/halfWidth [4] bitangent/halfHeight [5] type/両面/予備
inline constexpr int kLegacyShapedLightStride = 6;

// PunctualShadowConstantsCB — HLSL の PunctualShadowConstants (b12) と 1 対 1 で対応する。
// LAYOUT: Assets/Shaders/Common/PunctualShadowConstants.hlsli と完全に一致させること。
struct PunctualShadowConstantsCB {
    math::Matrix4 punctualShadowVP[kMaxPunctualShadows];
    math::Vector4 punctualShadowRect[kMaxPunctualShadows];
    // x = NDC 深度バイアス, y = 影の濃さ [0,1], zw = 予備。
    math::Vector4 punctualShadowParams[kMaxPunctualShadows];

    math::Matrix4 lightCookieVP[kMaxLightCookies];
    math::Vector4 lightCookieRect[kMaxLightCookies];

    // レガシー経路 (b3 の固定長配列) 用のスロット番号。x = shadowIndex, y = cookieIndex。
    //   [0..7]  → LightConstantsCB::pointLights[0..7]
    //   [8..11] → LightConstantsCB::spotLights[0..3]
    // LightConstants の HLSL 定義は 4 か所へ手書きで複製されており、1 つ漏らすと
    // そのシェーダーだけ全ライトが別オフセットを読む。番号だけ逃がして b3 は不変にする。
    math::Vector4 legacyPunctualSlots[kMaxLegacyPunctualLights];

    // レガシー経路用の「大きさを持つ光源」の実体。1 本あたり kLegacyShapedLightStride レジスタ。
    // b3 には Area / Sphere / Tube の型そのものが無いので、番号ではなく実体を載せる。
    math::Vector4 legacyShapedLight[kMaxLegacyShapedLights * kLegacyShapedLightStride];

    float         punctualShadowTexel[2];
    int32_t       punctualShadowCount = 0;
    int32_t       punctualShadowPcf   = 1;

    float         lightCookieTexel[2];
    int32_t       lightCookieCount       = 0;
    int32_t       legacyShapedLightCount = 0;

    math::Vector4 _punctualShadowPad0{};
};
static_assert(sizeof(PunctualShadowConstantsCB) == 2800,
    "PunctualShadowConstantsCB must match PunctualShadowConstants in "
    "Common/PunctualShadowConstants.hlsli (2800 bytes)");

// FroxelFogCB — FroxelFogConstants.hlsli の FroxelFogConstants (b13) と一致させること。
struct FroxelFogCB {
    math::Matrix4 invViewProj;

    math::Vector3 cameraPos;  float nearDistance = 0.1f;
    math::Vector3 albedo;     float farDistance  = 64.0f;
    math::Vector3 emissive;   float density      = 0.02f;

    float    anisotropy    = 0.4f;
    float    heightFalloff = 0.0f;
    float    heightStart   = 0.0f;
    // スライス内のサンプル位置ずらし [0,1)。グリッドが粗いことによる
    // 「霧の中の板」をフレーム間のちらつきへ散らすためのディザ。
    // 単独で使うとちらつきがそのまま残る。必ず historyBlend の蓄積と対で使う。
    float    jitter        = 0.0f;

    uint32_t gridX = 0;
    uint32_t gridY = 0;
    uint32_t gridZ = 0;
    float    ambient = 1.0f;

    // 前フレームのビュー射影。フロクセルのワールド座標を前フレームのグリッドへ
    // 投影し直して履歴を引くのに使う (カメラが動いても履歴が付いてくる)。
    math::Matrix4 prevViewProj;
    // 今フレームの寄与率。0 で履歴のみ、1 で蓄積なし (= 生のちらつき)。
    float    historyBlend = 1.0f;
    // 履歴が使えないフレーム (初回 / 解像度変更 / 霧の再有効化) の印。
    uint32_t historyValid = 0;
    float    _pad[2]      = { 0.0f, 0.0f };
};
static_assert(sizeof(FroxelFogCB) == 224,
    "FroxelFogCB must match FroxelFogConstants in Common/FroxelFogConstants.hlsli (224 bytes)");

// フロクセル霧がフレームをまたいで持ち越す状態。実体はビュー単位で確保する。
// グリッドは視錐台に貼り付くので、共有すると互いのボリュームを上書きし合い、
// 相手のカメラ行列で履歴を引き直して霧が明滅する (prevViewProjection と同じ理由)。
struct FroxelFogViewState {
    // 履歴の引き直しに使う前フレームのビュー射影。
    math::Matrix4 prevViewProjection = math::Matrix4::Identity();
    // 前フレームのグリッド寸法。変わったフレームは履歴を捨てる (0 は「無効」の印)。
    uint32_t      grid[3]     = { 0u, 0u, 0u };
    // スライス内サンプル位置のディザ列の位置。
    uint32_t      jitterIndex = 0u;
    // 散乱ボリューム 2 枚のどちらへ書くか。毎フレーム反転する。
    bool          ping        = false;
};

// 自動露出のヒストグラムのビン数。
// LAYOUT: PostProcess/Color/ExposureCommon.hlsli の FBZZ_EXPOSURE_BINS と一致させること。
inline constexpr uint32_t kExposureHistogramBins = 256;

// AutoExposureCB — ExposureCommon.hlsli の AutoExposureConstants (b2) と一致させること。
struct AutoExposureCB {
    float    minEV        = -6.0f;
    float    evRange      = 20.0f;
    float    lowPercent   = 0.45f;
    float    highPercent  = 0.95f;

    float    speedUp      = 3.0f;
    float    speedDown    = 1.0f;
    float    deltaTime    = 0.0f;
    float    compensation = 0.0f;

    float    minEVClamp   = -8.0f;
    float    maxEVClamp   =  8.0f;
    // 1 = 順応を飛ばして即座に合わせる。初回フレームとシーン切り替えで立てる。
    uint32_t reset        = 1;
    float    _pad0        = 0.0f;
};
static_assert(sizeof(AutoExposureCB) == 48, "AutoExposureCB size mismatch");

// CookieBlitCB — CookieBlit.hlsl の CookieBlitConstants (b2) と一致させること。
struct CookieBlitCB {
    float    cookieRotation = 0.0f;  // [rad]
    uint32_t cookieSrgb     = 0;     // 1 = 元テクスチャが sRGB エンコード
    float    _cookiePad[2]  = { 0.0f, 0.0f };
};
static_assert(sizeof(CookieBlitCB) == 16, "CookieBlitCB size mismatch");

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
    // LUT / 天候 (weather* は LUT ブロックの空き 3 枠を流用。Constants.hlsli 側も同じ)
    float lutBlend;           float weatherWetness;      float weatherDarkening;   float weatherPuddle;
    // Reprojection 行列 (TAA / Motion Blur 共用)
    math::Matrix4 prevViewProjection;
    math::Matrix4 invPrevViewProjection;
    // Volumetric Lighting (拡張分)
    float volMinDist;         float volDensity;          float volHeightFalloff;   float volHeightStart;
    float volTintR;           float volTintG;            float volTintB;           float volEdgeFade;
    // 自動露出。autoExposureKey <= 0 で無効 (Composite が b5 の exposure をそのまま使う)。
    float autoExposureKey = 0.0f;
    float autoExposureCompensation = 0.0f;
    float autoExposureMinEV = -8.0f;
    float autoExposureMaxEV =  8.0f;
    // Forward のマテリアルが画面空間 AO / 接触影をどれだけ受けるか。0 で引かない。
    // Deferred では DeferredLighting が適用するので 0 を入れる (二重適用の防止)。
    float screenAoStrength = 0.0f;
    float screenContactShadowStrength = 0.0f;
    // バッファ解像度 / 描画解像度。AO と接触影は半解像度で焼かれるので 0.5。
    float screenAoScale = 1.0f;
    float screenContactShadowScale = 1.0f;
};
static_assert(sizeof(AdvancedGraphicsCB) == 352,
    "AdvancedGraphicsCB must match AdvancedGraphicsConstants in Constants.hlsli (352 bytes)");

/// Bloom のミップ連鎖の段数。連鎖の 1 段目が半解像度で、以降 1/2 ずつ。
/// 1080p で 5 段なら最小段は 33px 相当 = 全解像度で半径およそ 100px のにじみになる。
/// 増やすほど広がるが、画面全体がぼんやりする方向へ倒れる。
inline constexpr uint32_t kBloomMipCount = 5;

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
    // Option の「明るさ」。colorFilter の 4 成分目の空きを使うのでレイアウトは変わらない。
    // 1.0 で無加工。PostProcCB を zero-init する場所では 0 (= 真っ黒) になるので、
    // Composite 以外のパスで読むなら必ず明示的に埋めること。
    float userBrightness;
    // 画面フェード — Composite パスの最終出力に適用する。alpha=0 で通常, 1 で全面フェード色。
    float screenFadeColor[3];
    float screenFadeAlpha;
    // 大気フォグ統合 (環境システム §3-3): フォグ色の出どころ。0=指数(従来), 1=大気散乱(エアリアル)。
    // WHY: 末尾に追加し既存フィールドのオフセットを変えない (HLSL PostProcConstants と一致)。
    float fogSource;
    // 放射ブラー (VFXScreenEffect)。画面中心から外へ引き伸ばす量 [0,1]。
    // 既存の padding を名前付きにするだけ。レイアウトを変えると Constants.hlsli の
    // 4 コピーを同時に直す必要があり、1 つ漏らすとそのプロジェクトだけ値がずれる。
    float radialBlur;
    float _fogPad[2];
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
    // Bloom のミップ連鎖。texelSize は「書き込み先」のテクセルサイズで、
    // こちらは「読み込み元」。段ごとに寸法が違うので両方要る。
    float bloomSrcTexel[2];
    // 輝度閾値を掛ける段 (1 = 掛ける)。連鎖の 1 段目だけ 1 にする。
    // WHY 毎段掛けないか: 段を降りるたびに閾値を引くと、暗い段から順に消えていき
    //     広がりが出ない。閾値は「何を光らせるか」の選別であって、ぼかしの一部ではない。
    float bloomApplyThreshold;
    // 1 = 書き込み先へ加算 (アップサンプルの途中段)、0 = 上書き。
    float bloomAdditive;
    // カスタムパスのパラメーター 4〜7。既存の customParameters (0〜3) は
    // 中ほどに埋まっていて伸ばせないので、続きを末尾へ足す。
    // WHY 4 本では足りないか: 太さ・揺れ・速さ・明るさで既に埋まる。色や閾値を
    //     持たせようとした時点で «効果を 2 つに割る» しかなくなり、パスが増える。
    float customParameters2[4];
    // カスタムパスの «走り方»。パラメーターと違い、書き手ではなくエンジンが埋める。
    //   x = 入力 UV のスケール (downscale の逆数)。縮小して走るときだけ 1 未満
    //   y = 今が何回目の反復か (0 起点) / z = 反復の総数
    //   w = 予約
    float customPassInfo[4];
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

// ObjectMaskConstants (b2) — Pipeline/Mask/ObjectMask*.hlsl と一致させること。
//   payload … そのままマスクへ書く RGBA (意味は書き手と読み手の取り決め)
//   flags.x … 1 = 手前に何かある画素を捨てる (visibleOnly)
struct ObjectMaskCB {
    math::Vector4 payload;
    math::Vector4 flags;
};
static_assert(sizeof(ObjectMaskCB) == 32, "ObjectMaskCB size mismatch");

// デカールの受信レイヤーフィルタが有効であることを示す DecalCB::flags のビット。
inline constexpr uint32_t kDecalFlagReceiverFilter = 1u;

// DecalConstants (b10) — Material/Decal/DecalCommon.hlsli と完全に一致させること。
// b2 は "MaterialConstants" として .mat のリフレクション対象なので、投影データを
// そこへ置くとデカールだけマテリアルを持てない例外になる (Common/Binding.hlsli 参照)。
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
    // OBB 投影は斜めの面でテクスチャが引き伸ばされて長い筋になる (壁の角をまたいだ
    // 着弾痕が「伸びた汚れ」に見える)。角度で薄めれば破綻する範囲がそのまま消える。
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
    // ランタイムの輪郭マスク (RGB=要求された色 / A=太さ)。エディタ選択のマスクとは
    // 別に持つ。あちらは «選ばれているか» の 1 ビットで、色を載せると成立しない。
    renderer::ResourceHandle<renderer::RenderTargetTag> objectMaskRT;
    renderer::ResourceHandle<renderer::RenderTargetTag> customPostProcessRT[2];
    renderer::ResourceHandle<renderer::RenderTargetTag> gbufferRT;

    // Bloom のミップ連鎖と、各段の実寸。テクセルサイズを CB へ入れるのに要る。
    // GetDimensions で引けなくはないが、書き込み先しか分からないため
    // 「読み込み元のテクセルサイズ」は CPU 側から渡す必要がある。
    renderer::ResourceHandle<renderer::TextureTag> bloomChain[kBloomMipCount];
    // 足し戻しの書き先。読みながら書けないので、ダウンサンプル用とは別に持つ。
    renderer::ResourceHandle<renderer::TextureTag> bloomUpChain[kBloomMipCount];
    uint32_t bloomChainWidth[kBloomMipCount]  = {};
    uint32_t bloomChainHeight[kBloomMipCount] = {};
    renderer::ResourceHandle<renderer::TextureTag> bloomHalf;   // = bloomChain[0]
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
    renderer::ResourceHandle<renderer::ShaderTag> objectMaskShader;
    renderer::ResourceHandle<renderer::ShaderTag> objectMaskSkinnedShader;
    // 全画面コピー。読みながら書けない場所で «今の絵» を退避するのに使う。
    renderer::ResourceHandle<renderer::ShaderTag> copyColorShader;
    // 縮小して走ったカスタムパスの結果を実寸へ戻す。
    renderer::ResourceHandle<renderer::ShaderTag> customComposeShader;
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
    renderer::ResourceHandle<renderer::ConstantBufferTag> objectMaskCB;

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

    // ---- モーションベクター ----
    // RG = 現 UV - 前フレーム UV、B = 書き込み済みフラグ。TAA / MotionBlur が t26 で読む。
    // RGBA16F を使うのは CreateRenderTarget にフォーマット引数が無く RG16F を作れないため。
    renderer::ResourceHandle<renderer::RenderTargetTag>   velocityRT;
    renderer::ResourceHandle<renderer::ShaderTag>         velocityShader;
    renderer::ResourceHandle<renderer::ShaderTag>         velocitySkinnedShader;

    // ---- Spot / Point シャドウ ----
    // Directional の CSM (shadowMapRT) とは別のアトラス。t28 へ束縛する。
    // CSM のタイル数は視錐台の分割で、こちらは影付きライトの本数で決まる。
    // 面積を奪い合わせると、ライトを 1 つ置いただけで遠景カスケードが粗くなる。
    renderer::ResourceHandle<renderer::RenderTargetTag>   punctualShadowRT;
    renderer::ResourceHandle<renderer::ConstantBufferTag> punctualShadowCB;

    // ---- ライト Cookie (投影テクスチャ) ----
    // 複数の Cookie を 1 枚へ敷き詰めたアトラス。t31 へ束縛する。
    // 1 回の PS 呼び出しの中でまとめて評価するので、ライトごとに差し替えられない。
    renderer::ResourceHandle<renderer::RenderTargetTag>   lightCookieRT;
    renderer::ResourceHandle<renderer::ShaderTag>         cookieBlitShader;
    renderer::ResourceHandle<renderer::ConstantBufferTag> cookieBlitCB;
    renderer::ResourceHandle<renderer::PipelineStateTag>  cookieBlitPSO;

    // ---- 自動露出 (眼の順応) ----
    // exposureHistogram は毎フレーム作り直す作業領域、exposureResult は
    // 「順応済みの平均輝度」1 要素でフレームをまたいで生き続ける状態。
    renderer::ResourceHandle<renderer::StructuredBufferTag> exposureHistogram;
    renderer::ResourceHandle<renderer::StructuredBufferTag> exposureResult;
    renderer::ResourceHandle<renderer::ShaderTag>           exposureHistogramCS;
    renderer::ResourceHandle<renderer::ShaderTag>           exposureAverageCS;
    renderer::ResourceHandle<renderer::ConstantBufferTag>   exposureCB;

    // ---- フロクセル ボリューメトリック フォグ ----
    // froxelScatter は散乱と消散の生値、froxelIntegrated は Z 積分後の
    // 「加算する光 (rgb) と背景の透過率 (a)」。Composite は後者だけを読む。
    renderer::ResourceHandle<renderer::TextureTag>        froxelScatter;
    renderer::ResourceHandle<renderer::TextureTag>        froxelScatterHistory;
    renderer::ResourceHandle<renderer::TextureTag>        froxelIntegrated;
    renderer::ResourceHandle<renderer::ShaderTag>         froxelInjectCS;
    renderer::ResourceHandle<renderer::ShaderTag>         froxelIntegrateCS;
    renderer::ResourceHandle<renderer::ConstantBufferTag> froxelFogCB;

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
    // 頂点バッファは共有しない。エミッターごとに DynamicVertexBufferPool から借りる
    // (共有 1 本だと DX12 で 2 個目以降の Update が 1 個目の Draw を壊す)。
    // インデックスは全エミッター共通のクワッド列で、生成後は書き換えないので共有してよい。
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
    // VS+PS: billboard 描画。合成モードは PSO 側だけで切り替えるためシェーダーは 1 本。
    renderer::ResourceHandle<renderer::ShaderTag>         particleGpuShader;
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

    // clusterCB と同じ内容で、供給モードだけ Linear に固定した版。
    // クラスタリストはメインカメラの視錐台に対して 1 回だけ作られるので、別視点の
    // パス (リフレクションプローブ) が引くとまったく別の場所のライトを拾う。
    renderer::ResourceHandle<renderer::ConstantBufferTag>   clusterLinearCB;

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
// 行列・カリング錐台・書き込み先タイル・バイアスを束ねてあるので、ShadowPass 側は
// 「タイルを選んで既存の caster 提出を回す」だけで済む。
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

// PunctualShadowView — Spot / Point シャドウアトラスのタイル 1 枚ぶんの描画情報。
// RenderSystem が毎フレーム組み立て、ShadowPass がタイルへ描き、
// GeometryPassHelpers が PunctualShadowConstantsCB へ転送する。
// ShadowCascade と別の型なのは、あちらが正射影でワールド距離が一意に決まるのに対し
// こちらは透視投影で深度によって変わるため。同じ型だと使ってよい欄が分からなくなる。
struct PunctualShadowView {
    math::Matrix4 viewProjection;
    // ライトビュー単体。caster を光源に近い順へ並べるための深度キー算出に使う。
    math::Matrix4 view;
    // ライト視点のワールド位置 (= ライトの位置)。
    math::Vector3 eyePos;
    // caster カリング用錐台 (viewProjection から抽出済み)。
    math::Frustum frustum;
    // アトラス上の位置。xy = UV オフセット, zw = UV スケール。
    math::Vector4 atlasRect;
    // アトラス上のピクセル矩形。ShadowPass が IRenderer::SetViewport へ渡す。
    uint32_t      viewportX    = 0;
    uint32_t      viewportY    = 0;
    uint32_t      viewportSize = 0;
    // 透視投影の深度レンジで正規化した NDC バイアスと影の濃さ。
    float         biasNDC        = 0.0f;
    float         shadowStrength = 1.0f;
    // 光源半径ぶんの半影の広がり [テクセル]。0 で従来どおり硬い縁。
    float         penumbraTexels = 0.0f;
    // 1 テクセルが張る角度 [rad] = 2*tan(halfFov) / タイル一辺。
    // 透視投影ではテクセルの覆うワールド距離が深度に比例するので固定値を持てない。
    // 角度なら深度に依存せず、caster の「半径 / 距離」と直接比べられる。
    float         texelAngularSize = 0.0f;
};

// LightCookieView — Cookie アトラスのタイル 1 枚。
// シャドウのスロットとは独立に割り当てる。影を落とさないライトにも Cookie は付けられる。
struct LightCookieView {
    math::Matrix4 viewProjection;
    // アトラス上の位置。xy = UV オフセット, zw = UV スケール。
    math::Vector4 atlasRect;
    // アトラス上のピクセル矩形の左上。サイズは kLightCookieTileSize 固定。
    uint32_t      viewportX = 0;
    uint32_t      viewportY = 0;
    // 焼き直しの要否を判定するキー。前フレームと同じならタイルをそのまま使う。
    std::string   sourcePath;
    float         rotationRad = 0.0f;
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
    // パスが直接 CameraComponent を読むと、Scene View や VFX プレビューでゲームカメラの
    // 設定が効いてしまい、エディタ上の見え方が編集対象と食い違う。
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
    // 平行投影か。画面高さ比は距離で割らないので、判定式が分岐する。
    bool  cullOrthographic        = false;
    // 距離計算用のカメラ前方ベクトル (深度距離モードでのみ使う)。同じ理由で事前計算する。
    math::Vector3 cullCameraForward = { 0.0f, 0.0f, 1.0f };

    uint32_t width = 0;
    uint32_t height = 0;
    bool selectionOutlineEnabled = false;
    // 今フレームに 1 件でも生きた輪郭要求があるか (RenderSettings::objectMaskRequests)。
    bool objectMaskEnabled = false;
    // 選択マスクへ UI 要素の矩形を追記する。UISystemContext を握っているのは
    // Viewport ごとの呼び出し元なので、パス側は「入っていれば呼ぶ」だけにする。
    std::function<void()> appendUISelectionMask;

    // TAA サブピクセルジッター (NDC 単位)。TAA が無効なフレームは 0。
    // ジッターが無いと静止カメラでは同じ絵に収束してアンチエイリアスにならない。
    // b0 を組む各パスが MakeCameraFrameCB へ渡す。
    float taaJitterNdcX = 0.0f;
    float taaJitterNdcY = 0.0f;

    renderer::LightConstantsCB lightData;

    // ---- クラスタライトカリング ----
    // punctualLights は b3 の固定長配列 (点 8 / スポット 4) と並行して構築される。
    // b3 は 20 以上のシェーダーが directional・ambient を読むために使っているので消せない。
    // 点光源とスポットだけをこちらへ逃がし、対応済みのパスから順に切り替える。
    std::vector<PunctualLightGPU> punctualLights;
    ClusterLightMode              clusterLightMode = ClusterLightMode::Legacy;
    bool                          clusterDebugHeatmap = false;

    // フロクセル霧のフレーム間状態。実体は描画中のビューが持つ。
    // 霧を走らせないフレームは null のことがある。
    FroxelFogViewState*           froxelFogState = nullptr;

    math::Matrix4               lightVP;
    // 影の光源視点。ビルボードを光源へ正対させる必要があるパス
    // (パーティクル自己影の密度積み) が lightVP の内訳を要求する。
    math::Matrix4               lightView;
    math::Vector3               lightEyePos;
    // GBuffer を使う不透明パイプラインが有効かどうか。
    // WHY: RenderSettings の Forward/Deferred 名ではなく、各パスが GBuffer 入力を読めるかを判定する。
    bool                        isDeferred  = false;
    bool                        ssaoEnabled = false;
    // GBuffer の深度が「この時点で書き終わっている」か。
    // Deferred では常に true、Forward では GBuffer プリパスを走らせたときだけ true。
    // isDeferred は「不透明を GBuffer でライティングするか」でこちらとは別。同一視すると
    // 接触影が hdrRT のまだ書かれていない深度を読む。
    bool                        gbufferDepthReady = false;
    // 前方描画のマテリアルへ渡す画面空間の遮蔽。無効ハンドルなら束縛しない。
    // AO は GTAO / SSAO のうち実際に走った方が入る (シェーダーからは区別しない)。
    renderer::ResourceHandle<renderer::TextureTag> screenAoTexture;
    renderer::ResourceHandle<renderer::TextureTag> screenContactShadowTexture;
    // ライト正射影の深度範囲で正規化済みの NDC バイアス。固定 NDC 値だとシーンスケール
    // 依存になるので、0.005 / depthRange としてワールド約 5mm 相当を保つ。
    float                       shadowBiasNDC  = 0.0f;
    float                       shadowStrength = 1.0f;  // LightComponent から流れてくる影の濃さ

    // ── カスケードシャドウ ──────────────────────────────────────────────────
    // 有効なのは先頭 shadowCascadeCount 本。1 のときは従来の単一シャドウマップと等価
    // (カスケード 0 がアトラス全面を占める) なので、パス側に分岐は要らない。
    ShadowCascade               shadowCascades[renderer::kMaxShadowCascades];
    int                         shadowCascadeCount = 1;

    // ── Spot / Point シャドウ ────────────────────────────────────────────────
    // 有効なのは先頭 punctualShadowViewCount 枚。Spot は 1 枚、Point は連続する
    // 6 枚 (キューブ面) を占める。0 のとき ShadowPass はアトラスをクリアするだけで戻る。
    PunctualShadowView          punctualShadowViews[kMaxPunctualShadows];
    int                         punctualShadowViewCount = 0;
    // アトラス全体の一辺 [px]。タイルサイズは これ / 4。
    uint32_t                    punctualShadowResolution = 0;

    // レガシーライト経路 (b3 の固定長配列) 用のスロット番号。-1 = 無し。
    //   [0..7]  → lightData.pointLights[0..7]
    //   [8..11] → lightData.spotLights[0..3]
    // b3 経路のシェーダーは PunctualLightGPU を読まないので、構造体に埋めた
    // shadowIndex / cookieIndex が届かない。ここを通さないと影が Forward+ でだけ出る。
    // 既定値は必ず -1。0 埋めだと「スロット 0」という有効な番号になり、影を持たない
    // ライトが他のライトの深度を引く。
    int legacyShadowSlots[kMaxLegacyPunctualLights] =
        { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    int legacyCookieSlots[kMaxLegacyPunctualLights] =
        { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };

    // ── ライト Cookie ────────────────────────────────────────────────────────
    // 有効なのは先頭 lightCookieViewCount 枚。LightCookiePass が焼き、
    // GeometryPassHelpers が PunctualShadowConstantsCB へ転送する。
    LightCookieView             lightCookieViews[kMaxLightCookies];
    int                         lightCookieViewCount = 0;

    // ── レガシー経路 (b3) 向けの「大きさを持つ光源」────────────────────────────
    // punctualLights から Area / Sphere / Tube を先頭 kMaxLegacyShapedLights 本まで
    // 写したもの。b3 はこれらの型を運べないため、b12 側へ実体ごと載せる。
    PunctualLightGPU            legacyShapedLights[kMaxLegacyShapedLights];
    int                         legacyShapedLightCount = 0;
    // b3 の点光源 / スポットの光源半径 [m]。添字は legacyShadowSlots と同じ。
    // WHY 別配列か: b3 の PointLightData は 32 バイトぴったりで 1 float も空きが無い。
    float                       legacySourceRadius[kMaxLegacyPunctualLights] = {};

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
    // パスコールバックが RenderPipeline を直接参照しないよう、コールバックで渡す。
    // DeclareResource で transient=true のリソースのみ有効。未設定なら空ハンドル。
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
