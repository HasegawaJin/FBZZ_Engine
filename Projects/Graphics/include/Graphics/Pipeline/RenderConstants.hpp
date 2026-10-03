/// @file    RenderConstants.hpp
/// @brief   RenderGraph 注入パスと各描画パスが共有する実行コンテキスト。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once

#include <Graphics/Renderer/Camera.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/LightSystem.hpp>
#include <Graphics/Renderer/RenderGraph.hpp>
#include <Graphics/Renderer/RenderSettings.hpp>
#include <Graphics/Renderer/ResourceHandle.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Renderer/RenderEnvironment.hpp>
#include <Graphics/Pipeline/EnvironmentResources.hpp>
#include <Graphics/Pipeline/PassResources.hpp>
#include <Graphics/Pipeline/OcclusionCuller.hpp>
#include <Math/Frustum.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>


namespace fbzz::renderer { struct RenderScene; }

namespace fbzz::renderer {


struct RenderPassContext;

/// @note UserRenderPassInjectionPoint — Script が追加するパスを既存パイプラインのどこへ挿入するかを表す。
/// @note RenderGraph は依存関係で実行順を決めるが、HDR へ ReadWrite する透明系パスは依存が同じになりやすい。
/// @note 明示的な挿入点を持たせ、Water / VFX / PostProcess 前処理の意図をコードから読めるようにする。
enum class UserRenderPassInjectionPoint : uint8_t {
    AfterOpaque,
    AfterTransparent,
    BeforePostProcess
};

/// @note GPU パーティクル CS が参照する流れ 1 本分 (96 bytes)。
/// @note LAYOUT: ParticleGpuSim.cs.hlsl の GpuFlowField と完全に一致させること。
/// @note 定数バッファでなく StructuredBuffer にするのは、cbuffer は固定長で
/// @note あふれた流れを黙って捨てるしかないため。SRV なら上限が消え、焼いた場のため
/// @note 太らせても b0 のオフセット (末尾追加のみが規約) は動かない。
struct GpuFlowField {
    math::Vector4 posRadius;     ///< @note xyz=ワールド位置, w=影響半径 (<=0 で無限)
    math::Vector4 dirStrength;   ///< @note xyz=流向/渦軸 (ワールド・正規化済み), w=流速 [m/s]
    math::Vector4 params;        ///< @note x=種類(FlowFieldType), y=falloffPower,
                                 /// @note z=noiseFrequency, w=noiseSpeed
    /// @name Baked 型のときだけ使う
    /// @{
    math::Vector4 fieldRotation; ///< @note ワールド → 場のローカルへ戻す逆回転 (xyzw = クォータニオン)
    math::Vector4 fieldExtents;  ///< @note xyz=ワールド半径 [m], w=予約 (旧 tightness)
    math::Vector4 fieldTile;     ///< @note x=アトラスのタイル番号 (<0 で無効), y=maxMagnitude, zw=予約
    /// @}
};

static_assert(sizeof(GpuFlowField) == 96,
    "GpuFlowField must match ParticleGpuSim.cs.hlsl (96 bytes)");

/// @note GPU パーティクル CS 用定数バッファ (b0) — 688 bytes, 16-byte aligned
struct GpuParticleEmitterCB {
    math::Vector3 emitterPos;
    float         deltaTime;
    math::Vector3 gravity;        ///< @note ParticleEmitterSettings::gravity [m/s^2]
    uint32_t      maxParticles;
    math::Vector4 colorStart;
    math::Vector4 colorEnd;
    uint32_t      spawnCount;       ///< @note 今フレームのスポーン数
    uint32_t      spawnOffset;      ///< @note リングバッファ書き込み先頭インデックス
    float         colorCurvePower;  ///< @note CPU と一致: pow(t, colorCurvePower)
    /// @note 旧 velocityDamping と同じ枠・同じ単位 [1/s]。流れへ寄る速さになった。
    float         flowCoupling;
    float         sizeStart;
    float         sizeEnd;
    float         sizeCurvePower;
    float         pad0;
    uint32_t      spriteColumns;
    uint32_t      spriteRows;
    uint32_t      spriteStartFrame;
    uint32_t      spriteEndFrame;
    /// @name ノイズモジュール + 流れ (末尾追加で既存オフセットを変えない)
    /// @{
    float         time;             ///< @note カールノイズのスクロールに使う経過時間
    float         noiseStrength;    ///< @note エミッター固有乱流の強さ (0 で無効)
    float         noiseFrequency;
    float         noiseSpeed;
    uint32_t      flowFieldCount;   ///< @note gParticleForces (SRV) の有効本数
    uint32_t      flipbookMode;
    float         flipbookFramesPerSecond;
    float         pad1;
    /// @note 流れは StructuredBuffer へ移った (2026-09-11)。枠は詰めず空けたまま残す。
    /// @note この配列は b0 の途中にあり、消すと後続の全オフセットがずれて HLSL 側の
    /// @note gViewProjection 以降が丸ごと別の値を読む。CB は «末尾追加のみ» が規約なので、
    /// @note 384 バイトの無駄よりレイアウトが動かないことを優先する。
    math::Vector4 reservedFlowFields[24];
    math::Vector4 curveFlags;       ///< @note x=size, y=velocity, z=gradient, w=frameBlend
    math::Vector4 sizeCurveKeys01;  ///< @note time0,value0,time1,value1
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
    /// @}
    /// @name over-lifetime モジュール追加分 (末尾追加で既存オフセットを変えない)
    /// @{
    /// @note 既存 curveFlags が埋まっているため 2 本目のフラグ束を持つ。
    math::Vector4 curveFlags2;          ///< @note x=rotation, y=drag, z/w=予約
    math::Vector4 rotationCurveKeys01;  ///< @note time0,value0,time1,value1
    math::Vector4 rotationCurveKeys23;
    math::Vector4 dragCurveKeys01;
    math::Vector4 dragCurveKeys23;
    math::Vector3 orbitalAxis;          ///< @note 正規化済み
    float         orbitalVelocity;
    float         radialVelocity;
    /// @note bit0 = spriteRandomStartFrame / bit1 = spriteRandomRow
    uint32_t      spriteRandomFlags;
    float         velocityPad0;
    float         velocityPad1;
    /// @}
    /// @name カーブ 8 キー化の追加分
    /// @{
    /// @note 既存の *Keys01/23 はオフセットを変えずキー 4〜7 を末尾へ足す。
    /// @note CB は「末尾追加のみ」が規約 (途中へ挿すと HLSL 側の全オフセットがずれる)。
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
    /// @note 実キー数。4 キー固定だった頃は不要だったが、8 キー化で「どこまでが有効か」を
    /// @note GPU 側も知らないと、末尾のダミーキーを踏んで CPU と違う値を返す。
    math::Vector4 curveKeyCounts;   ///< @note x=size, y=velocity, z=rotation, w=drag
    /// @note 補間モード (0=Linear, 1=Step, 2=Smooth)。CPU の ApplyCurveInterpolation と対。
    math::Vector4 curveModes;       ///< @note x=size, y=velocity, z=rotation, w=drag
    math::Vector4 gradientMeta;     ///< @note x=キー数, y=補間モード, z/w=予約
    /// @}
};
/// @note 内訳: 従来 784 に over-lifetime 追加 112 (float4×5=80、orbitalAxis 12+orbitalVelocity 4=16、radialVelocity 4
/// @note spriteRandomFlags 4、pad 4×2=16) とカーブ 8 キー化 256 (float4×16: curve 4 本×2、gradient 1、色 4、meta 3) を足す。
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
    /// @note WaterRenderPass だけが waterSsrEnabled として使う枠。他パスは 0 のまま。
    float         _reserved;
    /// @note 平行投影なら 1、遠近投影なら 0。
    /// @note カメラ視点の projection / viewProjection は Reversed-Z (near → 1、far → 0)。
    /// @note 深度バッファの値と視空間 Z の関係が射影で変わるため、Space.hlsli の
    /// @note LinearizeDepthAuto がこれで式を切り替えないと SSAO/SSR/コンタクトシャドウ/
    /// @note ソフトパーティクル/デカールが正投影ビューで一斉に破綻する。
    float         isOrthographic;
    float         _pad;
};

/// @note TAA サブピクセルジッターを織り込んだ GPU 用射影行列 (Reversed-Z) を返す。
/// @param jitterNdcX,jitterNdcY NDC 単位のジッター量。TAA 非有効時は 0 を渡す。
/// @note ジッターはラスタライズする行列にだけ乗せる。カリング用の錐台には載せないこと
/// @note (半ピクセルのために可視判定を揺らす意味がない)。
/// @note 深度は near → 1、far → 0。カメラ視点の深度バッファ (reversedZ な RT) と対で使う。
inline math::Matrix4 MakeJitteredProjection(const renderer::Camera& camera,
                                            float jitterNdcX, float jitterNdcY)
{
    math::Matrix4 projection = camera.GetGpuProjectionMatrix();
    if (camera.m_projection == renderer::ProjectionMode::Orthographic) {
        /// @note 正投影は clip.w が常に 1 なので、平行移動成分へ直接足す。
        /// @note 透視と同じ m[*][2] へ足すと、ずれ量が視空間 Z に比例してしまう。
        projection.m[0][3] += jitterNdcX;
        projection.m[1][3] += jitterNdcY;
        return projection;
    }
    /// @note 列ベクトル規約 (clip = P * viewPos) なので、m[0][2] / m[1][2] に足すと
    /// @note clip.xy += jitter * clip.w となり、深度に依らない一定のピクセルずれになる。
    projection.m[0][2] += jitterNdcX;
    projection.m[1][2] += jitterNdcY;
    return projection;
}

/// @note ジッター込みの ViewProjection。b0 を使わず自前で行列を組むパス用。
inline math::Matrix4 MakeJitteredViewProjection(const renderer::Camera& camera,
                                                float jitterNdcX, float jitterNdcY)
{
    return MakeJitteredProjection(camera, jitterNdcX, jitterNdcY) * camera.GetViewMatrix();
}

/// @note カメラと TAA サブピクセルジッターから b0 (PerFrameCB) を組む。
/// @note invViewProjection をジッター込みの viewProjection から作るのは、深度バッファを
/// @note 焼いた射影と揃えないと復元したワールド座標がずれるため。逆に再投影先の
/// @note prevViewProjection 側はジッターを載せない — 履歴バッファはピクセル中心で
/// @note 収束した絵なので、そこへはピクセル中心座標で引く必要がある。
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
    /// @note x = LOD ディザのしきい値。既定 0 は「遷移していない」= 全画素を描く。
    /// @note LAYOUT: Constants.hlsli の ObjectConstants と一致させること。
    math::Vector4 objectParams;
};

/// @note LAYOUT: Assets/Shaders/Common/ClusterConstants.hlsli と完全に一致させること。
/// @note 片方だけ変えるとライトが黙って別の位置・別の色で評価される。

/// @note グリッドは解像度非依存の固定分割。
/// @note 画面ピクセル数からタイル数を決めると、ビューポートをリサイズするたびに
/// @note クラスタバッファを作り直すことになる。固定分割なら確保は起動時 1 回で済む。
inline constexpr uint32_t kClusterGridX = 32;
inline constexpr uint32_t kClusterGridY = 18;
inline constexpr uint32_t kClusterGridZ = 24;
inline constexpr uint32_t kClusterCount = kClusterGridX * kClusterGridY * kClusterGridZ;

/// @note 1 クラスタが保持できるライト数。あふれた分はライト番号の昇順で切り捨てる (決定的)。
/// @note 屋内で range が部屋と同程度のライトを数十本置くと 32 では足りず、番号の大きい
/// @note ライトが黙って消え、境界では出入りが点滅して見える。詳細は HLSL 側の comment。
inline constexpr uint32_t kMaxLightsPerCluster = 64;
/// @note クラスタ 1 個分の uint 数。先頭がライト数、続けてライト番号が並ぶ。
inline constexpr uint32_t kClusterStride = kMaxLightsPerCluster + 1;
/// @note 点光源 + スポットを統合した配列の上限 (従来は点 8 / スポット 4 だった)。
inline constexpr uint32_t kMaxPunctualLights = 256;

/// @note ライト供給モード。ClusterConstants.hlsli の FBZZ_LIGHT_MODE_* と一致させること。
/// @note cbuffer 未束縛のパスは中身が全ゼロで読まれるため 0 = Legacy にしておくと、
/// @note b9 と t29/t30 を渡していない既存パスは 1 ビットも変わらず動く。
enum class ClusterLightMode : uint32_t {
    Legacy    = 0, ///< @note b3 の固定長 cbuffer (点 8 / スポット 4)
    Linear    = 1, ///< @note StructuredBuffer を全数走査 (カリング無効・A/B 検証用)
    Clustered = 2, ///< @note クラスタが持つライトだけ走査
};

/// @note PunctualLightGPU::type。ClusterConstants.hlsli の FBZZ_LIGHT_TYPE_* と一致させること。
enum class PunctualLightType : uint32_t {
    Point = 0,
    Spot  = 1,
    /// @note 矩形の面光源。position が面の中心、direction が面の法線、tangent / bitangent が
    /// @note 面内の軸で、halfWidth / halfHeight がその半寸法。
    Area  = 2,
    /// @note 球の光源。position が中心、halfWidth が半径。
    Sphere = 3,
    /// @note カプセルの光源 (蛍光灯・ネオン管)。tangent が軸、halfWidth が半径、
    /// @note halfHeight が軸方向の半長。
    Tube   = 4,
};

/// @note 点光源 / スポット / 面光源を統合した 1 本ぶん (96 bytes)。
/// @note 統合するとインデックス空間が 1 本になり、カリング CS も PS も 1 重ループで済む。
/// @note shadowIndex / cookieIndex は punctualShadowVP / lightCookieVP の何番目か。-1 で無効。
/// @note 番号で参照するので、ライト 256 本に対し影は 16 本まで、という配分が CB を太らせない。
struct PunctualLightGPU {
    math::Vector3 position;  float    range;
    math::Vector3 color;     float    intensity;
    math::Vector3 direction; float    innerCos;   ///< @note Spot のみ
    float         outerCos;  uint32_t type;       int32_t shadowIndex = -1;
                                                  int32_t cookieIndex = -1;
    math::Vector3 tangent;   float    halfWidth;  ///< @note Area のみ
    math::Vector3 bitangent; float    halfHeight; ///< @note Area のみ
};
static_assert(sizeof(PunctualLightGPU) == 96,
    "PunctualLightGPU must match PunctualLight in Common/ClusterConstants.hlsli (96 bytes)");

/// @note シャドウを持てる Spot / Point の合計タイル数。Spot は 1 枚、Point はキューブ 6 面。
/// @note LAYOUT: PunctualShadowConstants.hlsli の FBZZ_MAX_PUNCTUAL_SHADOWS と一致させること。
inline constexpr int kMaxPunctualShadows = 16;
/// @note Cookie アトラスのタイル数。FBZZ_MAX_LIGHT_COOKIES と一致させること。
inline constexpr int kMaxLightCookies = 8;
/// @note Cookie アトラスの寸法。4 列 x 2 行 = kMaxLightCookies タイル。
/// @note 8 タイルをちょうど埋める比が 4:2 なので、正方形にすると半分が未使用のまま常駐する。
inline constexpr uint32_t kLightCookieTileSize    = 512u;
inline constexpr uint32_t kLightCookieAtlasCols   = 4u;
inline constexpr uint32_t kLightCookieAtlasRows   = 2u;
inline constexpr uint32_t kLightCookieAtlasWidth  = kLightCookieTileSize * kLightCookieAtlasCols;
inline constexpr uint32_t kLightCookieAtlasHeight = kLightCookieTileSize * kLightCookieAtlasRows;
static_assert(kLightCookieAtlasCols * kLightCookieAtlasRows
                  == static_cast<uint32_t>(kMaxLightCookies),
              "cookie atlas tiling must cover exactly kMaxLightCookies tiles");
/// @note レガシー経路 (b3) が運べるライト数 = 点 8 + スポット 4。
/// @note LAYOUT: Common/Constants.hlsli の MAX_POINT_LIGHTS + MAX_SPOT_LIGHTS と一致させること。
inline constexpr int kMaxLegacyPunctualLights = 12;
/// @note legacyPunctualSlots 内でスポットが始まる位置。
inline constexpr int kLegacySpotSlotBase = 8;
/// @note レガシー経路 (b3) が型を運べない「大きさを持つ光源」(Area / Sphere / Tube) を
/// @note b12 側へ載せる本数。
/// @note LAYOUT: PunctualShadowConstants.hlsli の FBZZ_MAX_LEGACY_SHAPED_LIGHTS と一致させること。
inline constexpr int kMaxLegacyShapedLights = 4;
/// @note 1 本が占める float4 の数。レイアウトは PunctualShadowConstants.hlsli が正本。
/// @note [0] position/range [1] color/intensity [2] direction/予備
/// @note [3] tangent/halfWidth [4] bitangent/halfHeight [5] type/両面/予備
inline constexpr int kLegacyShapedLightStride = 6;

/// @note PunctualShadowConstantsCB — HLSL の PunctualShadowConstants (b12) と 1 対 1 で対応する。
/// @note LAYOUT: Assets/Shaders/Common/PunctualShadowConstants.hlsli と完全に一致させること。
struct PunctualShadowConstantsCB {
    math::Matrix4 punctualShadowVP[kMaxPunctualShadows];
    math::Vector4 punctualShadowRect[kMaxPunctualShadows];
    /// @note x = NDC 深度バイアス, y = 影の濃さ [0,1], zw = 予備。
    math::Vector4 punctualShadowParams[kMaxPunctualShadows];

    math::Matrix4 lightCookieVP[kMaxLightCookies];
    math::Vector4 lightCookieRect[kMaxLightCookies];

    /// @note レガシー経路 (b3 の固定長配列) 用のスロット番号。x = shadowIndex, y = cookieIndex。
    /// @note [0..7]  → LightConstantsCB::pointLights[0..7]
    /// @note [8..11] → LightConstantsCB::spotLights[0..3]
    /// @note LightConstants の HLSL 定義は 4 か所へ手書きで複製されており、1 つ漏らすと
    /// @note そのシェーダーだけ全ライトが別オフセットを読む。番号だけ逃がして b3 は不変にする。
    math::Vector4 legacyPunctualSlots[kMaxLegacyPunctualLights];

    /// @note レガシー経路用の「大きさを持つ光源」の実体。1 本あたり kLegacyShapedLightStride レジスタ。
    /// @note b3 には Area / Sphere / Tube の型そのものが無いので、番号ではなく実体を載せる。
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

/// @note FroxelFogCB — FroxelFogConstants.hlsli の FroxelFogConstants (b13) と一致させること。
struct FroxelFogCB {
    math::Matrix4 invViewProj;

    math::Vector3 cameraPos;  float nearDistance = 0.1f;
    math::Vector3 albedo;     float farDistance  = 64.0f;
    math::Vector3 emissive;   float density      = 0.02f;

    float    anisotropy    = 0.4f;
    float    heightFalloff = 0.0f;
    float    heightStart   = 0.0f;
    /// @note スライス内のサンプル位置ずらし [0,1)。グリッドが粗いことによる
    /// @note 「霧の中の板」をフレーム間のちらつきへ散らすためのディザ。
    /// @note 単独で使うとちらつきがそのまま残る。必ず historyBlend の蓄積と対で使う。
    float    jitter        = 0.0f;

    uint32_t gridX = 0;
    uint32_t gridY = 0;
    uint32_t gridZ = 0;
    float    ambient = 1.0f;

    /// @note 前フレームのビュー射影。フロクセルのワールド座標を前フレームのグリッドへ
    /// @note 投影し直して履歴を引くのに使う (カメラが動いても履歴が付いてくる)。
    math::Matrix4 prevViewProj;
    /// @note 今フレームの寄与率。0 で履歴のみ、1 で蓄積なし (= 生のちらつき)。
    float    historyBlend = 1.0f;
    /// @note 履歴が使えないフレーム (初回 / 解像度変更 / 霧の再有効化) の印。
    uint32_t historyValid = 0;
    float    _pad[2]      = { 0.0f, 0.0f };
};
static_assert(sizeof(FroxelFogCB) == 224,
    "FroxelFogCB must match FroxelFogConstants in Common/FroxelFogConstants.hlsli (224 bytes)");

/// @note フロクセル霧がフレームをまたいで持ち越す状態。実体はビュー単位で確保する。
/// @note グリッドは視錐台に貼り付くので、共有すると互いのボリュームを上書きし合い、
/// @note 相手のカメラ行列で履歴を引き直して霧が明滅する (prevViewProjection と同じ理由)。
struct FroxelFogViewState {
    /// @note 履歴の引き直しに使う前フレームのビュー射影。
    math::Matrix4 prevViewProjection = math::Matrix4::Identity();
    /// @note 前フレームのグリッド寸法。変わったフレームは履歴を捨てる (0 は「無効」の印)。
    uint32_t      grid[3]     = { 0u, 0u, 0u };
    /// @note スライス内サンプル位置のディザ列の位置。
    uint32_t      jitterIndex = 0u;
    /// @note 散乱ボリューム 2 枚のどちらへ書くか。毎フレーム反転する。
    bool          ping        = false;
};

/// @note 自動露出のヒストグラムのビン数。
/// @note LAYOUT: PostProcess/Color/ExposureCommon.hlsli の FBZZ_EXPOSURE_BINS と一致させること。
inline constexpr uint32_t kExposureHistogramBins = 256;

/// @note AutoExposureCB — ExposureCommon.hlsli の AutoExposureConstants (b2) と一致させること。
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
    /// @note 1 = 順応を飛ばして即座に合わせる。初回フレームとシーン切り替えで立てる。
    uint32_t reset        = 1;
    float    _pad0        = 0.0f;
};
static_assert(sizeof(AutoExposureCB) == 48, "AutoExposureCB size mismatch");

/// @note CookieBlitCB — CookieBlit.hlsl の CookieBlitConstants (b2) と一致させること。
struct CookieBlitCB {
    float    cookieRotation = 0.0f;  ///< @note [rad]
    uint32_t cookieSrgb     = 0;     ///< @note 1 = 元テクスチャが sRGB エンコード
    float    _cookiePad[2]  = { 0.0f, 0.0f };
};
static_assert(sizeof(CookieBlitCB) == 16, "CookieBlitCB size mismatch");

struct ClusterConstantsCB {
    /// @note 1 クラスタが覆う画面上のピクセル数 = screenSize / (GridX, GridY)。
    /// @note CPU 側で割るのは、マテリアルパスが PostProcConstants (b5) を束縛せず
    /// @note シェーダー側で screenSize を参照できないため。
    float    clusterTilePx[2];
    float    clusterSliceScale;
    float    clusterSliceBias;
    uint32_t clusterLightMode;   ///< @note ClusterLightMode
    uint32_t punctualLightCount;
    uint32_t clusterDebugMode;   ///< @note 0=通常, 1=クラスタあたりライト数のヒートマップ
    uint32_t _clusterPad0 = 0;
};
static_assert(sizeof(ClusterConstantsCB) == 32,
    "ClusterConstantsCB must match ClusterConstants in Common/ClusterConstants.hlsli (32 bytes)");

/// @note ShadowConstantsCB — HLSL の ShadowConstants (b4) と 1 対 1 で対応する。
/// @note LAYOUT: Assets/Shaders/Common/ShadowConstants.hlsli と完全に一致させること。
/// @note あちらが唯一の HLSL 側定義 (Constants / Terrain / Water が include する)。
struct ShadowConstantsCB {
    /// @note 単一のライト行列で足りるパス向け (= cascadeViewProjection[0] と同じ内容)。
    /// @note パーティクル自己影・体積光など、カスケードの概念を持たない経路が使う。
    math::Matrix4 lightViewProjection;
    /// @note カスケードごとのライト viewProjection。有効なのは先頭 cascadeCount 本。
    math::Matrix4 cascadeViewProjection[renderer::kMaxShadowCascades];
    /// @note カスケードごとのアトラス矩形。xy = UV オフセット, zw = UV スケール。
    math::Vector4 cascadeAtlasRect[renderer::kMaxShadowCascades];
    /// @note カスケードごとの NDC 深度バイアス (x=cascade0 .. w=cascade3)。
    /// @note カスケードごとに正射影の深度レンジが違うため、同じワールド距離のオフセットでも
    /// @note NDC 換算値が変わる。1 つの値を共有すると必ずどこかで破綻する。
    math::Vector4 cascadeBias;

    float         shadowMapTexelSize[2]; ///< @note 1.0 / アトラス全体の解像度
    float         shadowBias;            ///< @note 単一カスケード時のバイアス (= cascadeBias.x)
    float         shadowStrength;        ///< @note 0=影なし, 1=完全な影

    int           shadowPcfRadius;  ///< @note PCF カーネル半径: 0=ハード, 1=3x3, 2=5x5, 3=7x7
    int           cascadeCount;     ///< @note 1 = 単一シャドウマップ (従来), 2〜4 = CSM
    float         cascadeBlend;     ///< @note カスケード境界のクロスフェード幅 [0,1]
    int           cascadeDebugView; ///< @note 1 = カスケード番号を色で可視化

    /// @note 雲シャドウ (Phase C)
    float         cloudShadowStrength; ///< @note 0=無効
    float         cloudShadowCoverage;
    float         cloudShadowScale;
    float         cloudShadowSpeed;

    float         cloudShadowTime;
    float         cloudShadowWindX;
    float         cloudShadowWindZ;
    float         _shadowPad0 = 0.0f;
    /// @note CSM の選択はライト空間の重なりでなく、このビューの前方距離で行う。
    math::Vector4 cascadeSplitFar;
    math::Vector4 shadowCameraPosition;
    math::Vector4 shadowCameraForward;
};
static_assert(sizeof(ShadowConstantsCB) == 512,
    "ShadowConstantsCB must match ShadowConstants in Common/ShadowConstants.hlsli (512 bytes)");

struct AtmosphereCB {
    float rayleighScattering[3];
    float mieScattering;
    float planetRadius;
    float atmosphereRadius;
    float sunIntensity;
    float mieG;
    /// @name 月 (Phase B) ── HLSL AtmosphereConstants と一致させること (末尾追加・16byte 整列)。
    /// @{
    float moonEnabled;     ///< @note 0/1
    float moonSize;
    float moonBrightness;
    float _moonPad0;
    float moonColor[3];
    float _moonPad1;
    /// @}
};

/// @brief Light Probe Volume 1 つぶんの配置。intensity <= 0 で無効。
/// @note LAYOUT: AdvancedGraphicsConstants.hlsli の ProbeVolumeParams と一致させること (48 bytes)。
struct ProbeVolumeParamsCB {
    math::Vector3 boxMin{};                     ///< @note 箱の最小角 [world]
    float         intensity = 0.0f;             ///< @note 拡散 GI の倍率
    math::Vector3 invSize{ 1.0f, 1.0f, 1.0f };  ///< @note 1 / 箱の大きさ [1/m]
    float         fade = 0.0f;                  ///< @note 箱の縁で外側へ戻していく幅 [m]
    uint32_t      grid[3] = { 1u, 1u, 1u };
    float         normalBias = 0.0f;            ///< @note 法線方向へずらして引く距離 [m]
};
static_assert(sizeof(ProbeVolumeParamsCB) == 48, "ProbeVolumeParamsCB must match ProbeVolumeParams (48 bytes)");

/// @note AdvancedGraphicsCB — IBL・SSR・TAA・GTAO・Contact Shadow 等の詳細設定。
/// @note LAYOUT: Constants.hlsli の AdvancedGraphicsConstants cbuffer と完全に一致させること。
/// @note 16-byte アライメント制約のため、各グループを 4 要素単位でまとめる。
struct AdvancedGraphicsCB {
    /// @note IBL
    float iblIntensity;       float iblDiffuseScale;    float iblSpecularScale;   int   iblMaxMipLevel;
    /// @note SSR
    float ssrMaxDistance;     float ssrThickness;        int   ssrSteps;           float ssrIntensity;
    /// @note Volumetric
    float volLightIntensity;  float volScattering;       int   volSteps;           float volMaxDist;
    /// @note TAA
    float taaFeedback;        float taaJitterX;          float taaJitterY;         float _taaPad;
    /// @note Motion Blur
    /// @note screenWidth/screenHeight は MotionBlur CS が b5 非バインド下で screenSize の代替として参照する
    float motionBlurStrength; int   motionBlurSamples;   float screenWidth;         float screenHeight;
    /// @note GTAO
    float gtaoIntensity;      float gtaoRadius;          int   gtaoSlices;         int   gtaoStepsPerSlice;
    /// @note Contact Shadows
    float contactShadowStrength; float contactShadowRayLen; int contactShadowSteps; float contactShadowThick;
    /// @note Lens Flare
    float lensFlareIntensity; int   lensFlareGhostCount; float lensFlareHaloWidth; float lensFlareDistort;
    /// @note PCSS
    float pcssLightRadius;    int   pcssEnabled;         float _pcssPad0;          float _pcssPad1;
    /// @note LUT / 天候 (weather* は LUT ブロックの空き 3 枠を流用。Constants.hlsli 側も同じ)
    float lutBlend;           float weatherWetness;      float weatherDarkening;   float weatherPuddle;
    /// @note Reprojection 行列 (TAA / Motion Blur 共用)
    math::Matrix4 prevViewProjection;
    math::Matrix4 invPrevViewProjection;
    /// @note Volumetric Lighting (拡張分)
    float volMinDist;         float volDensity;          float volHeightFalloff;   float volHeightStart;
    float volTintR;           float volTintG;            float volTintB;           float volEdgeFade;
    /// @note 自動露出。autoExposureKey <= 0 で無効 (Composite が b5 の exposure をそのまま使う)。
    float autoExposureKey = 0.0f;
    float autoExposureCompensation = 0.0f;
    float autoExposureMinEV = -8.0f;
    float autoExposureMaxEV =  8.0f;
    /// @note Forward のマテリアルが画面空間 AO / 接触影をどれだけ受けるか。0 で引かない。
    /// @note Deferred では DeferredLighting が適用するので 0 を入れる (二重適用の防止)。
    float screenAoStrength = 0.0f;
    float screenContactShadowStrength = 0.0f;
    /// @note バッファ解像度 / 描画解像度。AO と接触影は半解像度で焼かれるので 0.5。
    float screenAoScale = 1.0f;
    float screenContactShadowScale = 1.0f;
    /// @note Light Probe Volume。[0] が内側 (t22)、[1] が外側 (t21)。
    /// @see Assets/Shaders/Rendering/LightProbeGI.hlsli
    ProbeVolumeParamsCB probeVolumes[2]{};
    float probeSpecularOcclusion = 0.0f;     ///< @note プローブの暗さを鏡面 IBL へ移す強さ [0,1]
    float _probePad[3]{};
};
static_assert(sizeof(AdvancedGraphicsCB) == 464,
    "AdvancedGraphicsCB must match AdvancedGraphicsConstants in AdvancedGraphicsConstants.hlsli (464 bytes)");

/// @note Bloom のミップ連鎖の段数。連鎖の 1 段目が半解像度で、以降 1/2 ずつ。
/// @note 1080p で 5 段なら最小段は 33px 相当 = 全解像度で半径およそ 100px のにじみになる。
/// @note 増やすほど広がるが、画面全体がぼんやりする方向へ倒れる。
inline constexpr uint32_t kBloomMipCount = 5;

/// @note Stored as float in the existing PostProcCB field; source and final resolve must not share boolean semantics.
enum class ReflectionResolveStage : uint32_t { LEGACY = 0, FINAL = 1, SOURCE = 2 };

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
    /// @note Option の「明るさ」。colorFilter の 4 成分目の空きを使うのでレイアウトは変わらない。
    /// @note 1.0 で無加工。PostProcCB を zero-init する場所では 0 (= 真っ黒) になるので、
    /// @note Composite 以外のパスで読むなら必ず明示的に埋めること。
    float userBrightness;
    /// @note 画面フェード — Composite パスの最終出力に適用する。alpha=0 で通常, 1 で全面フェード色。
    float screenFadeColor[3];
    float screenFadeAlpha;
    /// @note 大気フォグ統合 (環境システム §3-3): フォグ色の出どころ。0=指数(従来), 1=大気散乱(エアリアル)。
    /// @note 末尾に追加し、既存フィールドのオフセット (HLSL PostProcConstants と一致) を変えない。
    float fogSource;
    /// @note 放射ブラー (VFXScreenEffect)。画面中心から外へ引き伸ばす量 [0,1]。
    /// @note 既存の padding を名前付きにするだけ。レイアウトを変えると Constants.hlsli の
    /// @note 4 コピーを同時に直す必要があり、1 つ漏らすとそのプロジェクトだけ値がずれる。
    float radialBlur;
    float _fogPad[2];
    /// @note 投影コースティクス改良 (Phase C-2): 水域 XZ 範囲フェード + 波連動 UV ゆらぎ。
    /// @note HalfExtent<=0 で範囲無制限 (後方互換)。WaveAmp=0 でゆらぎ無し。
    float causticsCenterX;
    float causticsCenterZ;
    float causticsHalfExtentX;
    float causticsHalfExtentZ;
    float causticsWaveAmp;
    float causticsWaveFreq;
    float causticsWaveSpeed;
    float _causticsPad;
    /// @note Bloom のミップ連鎖。texelSize は「書き込み先」のテクセルサイズで、
    /// @note こちらは「読み込み元」。段ごとに寸法が違うので両方要る。
    float bloomSrcTexel[2];
    /// @note 輝度閾値を掛ける段 (1 = 掛ける)。連鎖の 1 段目だけ 1 にする。
    /// @note 毎段掛けると暗い段から順に消えて広がりが出ない。閾値は「何を光らせるか」の
    /// @note 選別であって、ぼかしの一部ではない。
    float bloomApplyThreshold;
    /// @note 1 = 書き込み先へ加算 (アップサンプルの途中段)、0 = 上書き。
    float bloomAdditive;
    /// @note カスタムパスのパラメーター 4〜7。既存の customParameters (0〜3) は
    /// @note 中ほどに埋まっていて伸ばせないので、続きを末尾へ足す。
    /// @note 4 本では太さ・揺れ・速さ・明るさで埋まる。色や閾値を持たせようとすると
    /// @note «効果を 2 つに割る» しかなくなり、パスが増える。
    float customParameters2[4];
    /// @note カスタムパスの «走り方»。パラメーターと違い、書き手ではなくエンジンが埋める。
    /// @note x = 入力 UV の横の倍率 (縮小後の幅 / 実寸の幅)。縮小して走るときだけ 1 未満
    /// @note y = 今が何回目の反復か (0 起点) / z = 反復の総数
    /// @note w = 入力 UV の縦の倍率 (縮小後の高さ / 実寸の高さ)。縦横は別々に切り捨てられるので x と一致しない
    /// @note シェーダーは Constants.hlsli の FBZZ_CustomInputUV で読む。
    float customPassInfo[4];
    /// @note 衝撃波リング (VFXScreenEffect)。Amplitude=0 で無効。
    /// @note 途中の padding に詰めると、Constants.hlsli の 4 コピーの直し忘れが
    /// @note «別の効果» を壊す。末尾なら直し忘れは «リングが出ない» だけで済む。
    float shockRingCenter[2];
    float shockRingRadius;
    float shockRingWidth;
    float shockRingAmplitude;
    /// @note 今フレーム生成したレイ反射を読むときだけ 1。既定 0 は未束縛 SRV を参照しない。
    float rayReflectionEnabled = 0.0f;
    /// @note ReflectionResolveStage: legacy0 / final1 / baseline source2. Primary glass replaces full surface radiance only in final.
    float reflectionResolveEnabled = 0.0f;
    /// @note このビューで SSR の記録に成功したときだけ読み、有効性と材質応答を分離する。
    float reflectionSsrEnabled = 0.0f;
};
/// @note HLSL 側 (Common/Constants.hlsli の PostProcConstants) は複数コピーある。サイズがずれたら全コピーを直す。
static_assert(sizeof(PostProcCB) == 416, "PostProcCB must match PostProcConstants in Constants.hlsli (416 bytes)");

/// @note 画面サイズ由来のフィールドだけを埋めた PostProcCB を返す。
/// @note b5 は全ポストプロセスで共有され、各パスが構造体ごと上書きする。texelSize/screenSize を
/// @note 入れ忘れたパスは前パス (あるいは前フレーム) の残りを読んで静かに壊れるので、
/// @note b5 を使うパスは必ずここを起点にしてから固有フィールドを足すこと。
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
static_assert(sizeof(OutlineCB) == 32, "OutlineCB must match OutlineConstants in SelectionOutline.hlsl (32 bytes)");

/// @note ObjectMaskConstants (b2) — Pipeline/Mask/ObjectMask*.hlsl と一致させること。
/// @note payload … そのままマスクへ書く RGBA (意味は書き手と読み手の取り決め)
/// @note flags.x … 1 = 手前に何かある画素を捨てる (visibleOnly)
struct ObjectMaskCB {
    math::Vector4 payload;
    math::Vector4 flags;
};
static_assert(sizeof(ObjectMaskCB) == 32, "ObjectMaskCB size mismatch");

/// @note デカールの受信レイヤーフィルタが有効であることを示す DecalCB::flags のビット。
inline constexpr uint32_t kDecalFlagReceiverFilter = 1u;

/// @note DecalConstants (b10) — Material/Decal/DecalCommon.hlsli と完全に一致させること。
/// @note b2 は "MaterialConstants" として .mat のリフレクション対象なので、投影データを
/// @note そこへ置くとデカールだけマテリアルを持てない例外になる (Common/Binding.hlsli 参照)。
struct DecalCB {
    math::Matrix4 invDecalWorld;
    math::Vector3 decalTangent;
    float         _pad0 = 0.0f;
    math::Vector3 decalBitangent;
    float         _pad1 = 0.0f;
    math::Vector3 decalNormal;
    float         _pad2 = 0.0f;
    float         alpha = 1.0f;
    /// @note 角度フェード。受け面の法線が投影軸から傾くほどデカールを薄くする。
    /// @note OBB 投影は斜めの面でテクスチャが引き伸ばされて長い筋になる (壁の角をまたいだ
    /// @note 着弾痕が「伸びた汚れ」に見える)。角度で薄めれば破綻する範囲がそのまま消える。
    float         angleFadeStrength = 1.0f; ///< @note 0 = フェードなし
    float         angleFadeCos      = 0.34f; ///< @note この cos より寝た面では完全に消える (既定 70 度)
    uint32_t      flags             = 0;
    uint32_t      receiverLayerMask = ~0u;
    /// @note フリップブック。1 コマぶんの UV スケールと、今のコマ番号。
    /// @note 無効時は (1, 1) / 0 で、シェーダー側の変換が恒等になる。
    /// @note «スケール + 番号» にするのは旧 _pad3 の 3 float に収まり cbuffer サイズが
    /// @note 変わらないため。伸ばすと DecalCommon.hlsli の 4 コピーと static_assert を
    /// @note 同時に直す必要があり、直し忘れると黙って別の値を読む。
    float         frameScale[2] = { 1.0f, 1.0f };
    float         frameIndex    = 0.0f;
};
static_assert(sizeof(DecalCB) == 144, "DecalCB size mismatch");

/// @note 組み込み Decal.hlsl の MaterialConstants (b2)。
/// @note .mat を割り当てていない DecalComponent へは DecalPass がこの形で値を流す。
/// @note LAYOUT: Assets/Shaders/Material/Decal/Decal.hlsl と完全に一致させること。
struct DecalMaterialCB {
    float    albedoTint[4]    = { 1.0f, 1.0f, 1.0f, 1.0f };
    float    emissiveColor[3] = { 1.0f, 1.0f, 1.0f };
    float    emissiveScale    = 0.0f;
    float    uvTiling[2]      = { 1.0f, 1.0f };
    float    uvOffset[2]      = { 0.0f, 0.0f };
    float    normalStrength   = 1.0f;
    /// @note Material::Upload と同じ規則: bit i = テクスチャスロット i が有効。
    /// @note bit0=albedo(t0) bit1=normal(t1) bit3=emissive(t3)
    uint32_t textureMask      = 0;
    float    _pad[2]          = { 0.0f, 0.0f };
};
static_assert(sizeof(DecalMaterialCB) == 64, "DecalMaterialCB size mismatch");

/// @note 受信レイヤーバッファを描くドローの b10。DecalMask(.Skinned).hlsl と一致させること。
struct DecalReceiverCB {
    /// @note レイヤー番号 + 1。0 はクリア値 (未描画) と衝突するため使わない。
    float layerEncoded = 0.0f;
    float _pad[3]      = { 0.0f, 0.0f, 0.0f };
};
static_assert(sizeof(DecalReceiverCB) == 16, "DecalReceiverCB size mismatch");

/// @note 公開ヘッダーに置く理由: Particle / Trail / Terrain / Water は «エンジンが埋める»
/// @note 定数バッファを持ち、材質の b2 とは別枠。src/ に閉じると、マテリアルプレビュー等
/// @note «本編と同じ絵を焼きたい» 側が写しを持つしかなく、正本が 2 つに分かれる。

/// @note パーティクルビルボード頂点。Particle.hlsl の ParticleVSIn と一致させること。
struct ParticleVertex {
    float center[3];  ///< @note POSITION   12 bytes
    float uv[2];      ///< @note TEXCOORD0   8 bytes
    float color[4];   ///< @note COLOR       16 bytes
    float size;       ///< @note TEXCOORD1    4 bytes
    float rotation;   ///< @note TEXCOORD2    4 bytes
    float uvRect[4];  ///< @note TEXCOORD3   16 bytes
    float velocity[3];///< @note TEXCOORD4   12 bytes
    float nextUvRect[4]; ///< @note TEXCOORD5 16 bytes
    float spriteBlend;   ///< @note TEXCOORD6  4 bytes
};                       ///< @note 92 bytes
static_assert(sizeof(ParticleVertex) == 92, "ParticleVertex must match ParticleVSIn (92 bytes)");

/// @note ParticleRenderCB::effectsFlags のビット割り当て。
/// @note LAYOUT: Assets/Shaders/Rendering/ParticleCommon.hlsli の FBZZ_PFX_* / FBZZ_PALPHA_* と
/// @note 完全に一致させること。片方だけ変えると該当機能が黙って効かなくなる。
inline constexpr std::uint32_t kParticleFxDistortion    = 1u;
inline constexpr std::uint32_t kParticleFxSixWay        = 2u;
inline constexpr std::uint32_t kParticleFxMotionVector  = 4u;
inline constexpr std::uint32_t kParticleFxReceiveShadow = 8u;
inline constexpr std::uint32_t kParticleFxVolumetric    = 16u;
/// @note 事前乗算アルファ。PS がソフトパーティクルの fade を RGB へも掛けるために使う。
inline constexpr std::uint32_t kParticleFxPremultiplied = 32u;
/// @note albedo テクスチャが sRGB エンコード。PS が SRGBToLinear を掛ける。
inline constexpr std::uint32_t kParticleFxSrgbTexture   = 64u;
/// @note 歪み専用ノーマルマップ (t1) がバインドされている。
inline constexpr std::uint32_t kParticleFxDistortionMap = 128u;
/// @note bit8-10 は下のアルファの取り出し方が使うので、以降の機能ビットは bit11 から。
/// @note 点光源 (クラスタ) を粒子の中心で受ける。
inline constexpr std::uint32_t kParticleFxPunctual      = 1u << 11;
/// @note 6 方向ライトマップ (t0 = Positive / t3 = Negative) で陰影を付ける。
inline constexpr std::uint32_t kParticleFxSixWayMaps    = 1u << 12;
/// @note 加算合成。霧の補正 (ParticleLighting.hlsli) と TAA の反応マスクが合成式によって式を変える。
inline constexpr std::uint32_t kParticleFxAdditive      = 1u << 13;
/// @note six-way の色相 / emission atlas を t10/t11 から読む。
inline constexpr std::uint32_t kParticleFxSixWayColorMaps = 1u << 14;
/// @note アルファの取り出し方は bit8-10 の 3 ビットに ParticleAlphaSource を格納する。
/// @note 値は Rendering/Mask.hlsli の FBZZ_MASK_* と共通 (全マテリアルで同じ語彙を使う)。
inline constexpr std::uint32_t kParticleAlphaShift = 8u;
inline constexpr std::uint32_t kParticleAlphaMask  = 7u;

/// @note ParticleRenderCB を束縛する DrawCall::constantBuffers のスロット。
/// @note LAYOUT: Assets/Shaders/Common/Binding.hlsli の CB_PARTICLE と一致させること。
/// @note b2 ではない理由: シェーダーリフレクションは cbuffer 名 "MaterialConstants" を
/// @note b2 に探すため、そこを占有すると .mat の [params] を束縛できなくなる。
/// @note b2 は材質へ明け渡す (Decal の CB_DECAL と同じ判断)。
inline constexpr std::size_t kParticleConstantSlot = 11;
/// @note six-way 色アトラスは Particle pass 専用で、Bloom / Environment map と時分割する。
inline constexpr std::size_t kParticleSixWayAlbedoColorSlot = 10;
inline constexpr std::size_t kParticleSixWayEmissionColorSlot = 11;

/// @note Particle描画専用CB (b11)。CPU/GPUシェーダーで同じRenderer設定を使う。
struct ParticleRenderCB {
    uint32_t renderMode = 0;
    float stretchedVelocityScale = 0.1f;
    float stretchedLengthScale = 1.0f;
    float softParticleFadeDistance = 0.5f;
    uint32_t softParticles = 0;
    uint32_t maxParticles = 0;
    uint32_t effectsFlags = 0; ///< @note bit0 distortion / bit1 six-way lighting / bit2 motion-vector flipbook
    float distortionStrength = 0.015f;
    float lightingStrength = 1.0f;
    float emissiveScale = 1.0f;
    float motionVectorStrength = 1.0f;
    /// @note 描画先の解像度 [px]。
    /// @note 歪みはシーンカラーを画面 UV でサンプルするが、screenSize は PostProcConstants
    /// @note 側にありパーティクル描画はそれをバインドしない。未設定だと UV が saturate で
    /// @note 右下隅へ張り付き、屈折でなくべた塗りになるため、解像度はここから渡す。
    float screenWidth = 1.0f;
    float screenHeight = 1.0f;
    /// @note ビルボードの軸ごとのサイズ倍率 (ParticleEmitter::sizeAxisScale の xy)。
    /// @note 縦横比はエミッター単位の値なので粒子ごとに持たせず CB で渡す。CPU 頂点
    /// @note フォーマットも GPU の GpuParticle も太らせずに、CPU/GPU 双方の描画へ
    /// @note 同じ 1 か所から効かせられる。
    float sizeAxisScaleX = 1.0f;
    float sizeAxisScaleY = 1.0f;
    /// @note 受け影の強さ [0,1]。有効/無効は effectsFlags の bit3 で判定する。
    float shadowStrength = 1.0f;
    /// @note ボリュメトリック煙 (effectsFlags bit4)。ビルボード内で球状密度場をレイマーチする。
    uint32_t volumetricSteps = 8;      ///< @note 視線方向のサンプル数
    float volumetricDensity = 1.0f;    ///< @note 消衰係数。大きいほど濃く不透明になる
    float volumetricAnisotropy = 0.3f; ///< @note Henyey-Greenstein g。正で前方散乱 (逆光で縁が光る)
    float volumetricNoiseScale = 2.0f; ///< @note 密度ノイズの空間周波数 [1/m]
    /// @note GPU ソート済みインデックス (t15) が有効か。GPU 経路の VS だけが読む。
    /// @note シェーダー分岐ではなく定数で切り替えるのは、ソート無効時に t15 へ何も
    /// @note バインドしない構成を許すため (未バインド SRV の読みを踏まない)。
    uint32_t gpuSortEnabled = 0;
    /// @note 自己影の消衰係数。0 で無効。光源側密度バッファ (t9) を引いて透過率へ変換する。
    /// @note 受け影は「他の物体が落とす影」しか扱えない。粒子群が自分へ落とす影が無いと、
    /// @note 厚みのある煙・雲は光の当たり方が一様になり平坦な塊に見える。
    float selfShadowStrength = 0.0f;
    /// @name 煙の散乱 (effectsFlags bit1 有効時)
    /// @{
    /// @note 素の N・L は不透明な球の陰影で、光を透かす媒質には合わない。巻き込み拡散で
    /// @note 陰側の黒潰れを避け、前方散乱で逆光時に縁が光るようにする。
    float smokeWrap = 0.5f;
    float smokeTransmission = 0.0f;
    /// @note HLSL の cbuffer では float4 が 16 バイト境界を跨げない。ここまでで offset 96 に
    /// @note 揃えてあるので、この 2 つを動かすとシェーダー側と黙ってずれる。
    math::Vector4 tintColor = { 1.0f, 1.0f, 1.0f, 1.0f }; ///< @note .mat の albedo (リニア済み)
    float smokeBackScatterPower = 4.0f;
    float distortionChromatic = 0.0f;
    /// @note カメラ距離フェード [m]。near 未満で 0、far 以上で 1 の不透明度になる。
    /// @note 0 / 0 (既定) で無効。near == far も無効扱い (0 除算になる)。
    /// @note 一人称の近距離で粒子が «顔に張り付いて画面を覆う» のを、粒子側のサイズや
    /// @note 寿命を変えずに消せる唯一の手段。pad 枠の転用なので CB サイズは動かない。
    float cameraFadeNear = 0.0f;
    float cameraFadeFar = 0.0f;
    math::Vector4 sixWayEmission = { 0.0f, 0.0f, 0.0f, 0.0f }; ///< @note rgb = 6-way マップの発光色 (リニア HDR)
    /// @}
};
static_assert(sizeof(ParticleRenderCB) == 144);

/// @note TrailVertex — Assets/Shaders/Material/Effects/Trail.hlsl の VS 入力と一致する CPU 頂点。
/// @note Trail ノード (TrailRenderPass) と per-particle Trail のリボン (ParticlePass) が共有する。
struct TrailVertex {
    math::Vector3 position;
    float         age;
    float         v;
    float         u;
};
static_assert(sizeof(TrailVertex) == 24, "TrailVertex layout mismatch");

/// @note TrailCB — TrailConstants (cbuffer b2) の C++ ミラー。
/// @note colorStart / colorEnd はリニアで入れること。sRGB のまま渡すと HDR バッファへ
/// @note sRGB 値を書くことになり、ACES を通した後で色が淡く飛ぶ。
struct TrailCB {
    math::Vector4 colorStart;
    math::Vector4 colorEnd;
    float uvScrollSpeed = 0.0f;
    float uvTiling = 1.0f;
    float time = 0.0f;
    /// @note bit0 = テクスチャが sRGB エンコード (シェーダー側でリニア化する)。
    std::uint32_t flags = 0;

    /// @note 多キー色 (TrailComponent::colorGradient)。gradientKeyCount = 0 で
    /// @note colorStart / colorEnd の 2 点へ落ちる。
    /// @note 頂点でなく CB に持つのは、TrailVertex が per-particle リボンと共有され
    /// @note 帯 1 本にしか要らない値を頂点数ぶん運ぶ理由が無いため。末尾に足すのは
    /// @note TrailCB を 0 初期化して sizeof で確保するパスが «キー無し» として素通りするため。
    math::Vector4 gradientColors[8]{}; ///< @note リニア化済み
    /// @note 8 個のキー時刻。float4 × 2 に詰めるのは、HLSL の cbuffer が float の配列を
    /// @note 1 要素 16 バイトへ膨らませるため (float times[8] は 128 バイトを食う)。
    math::Vector4 gradientTimes[8 / 4]{};
    std::uint32_t gradientKeyCount = 0;
    /// @note ParticleCurveInterpolation の値 (0=Linear / 1=Step / 2=Smooth)。
    std::uint32_t gradientInterpolation = 0;
    std::uint32_t _gradientPad[2]{};
};
/// @note Trail.hlsl の gTrailFlags と一致させること。
inline constexpr std::uint32_t kTrailFlagSrgbTexture = 1u;

/// @note テクスチャが sRGB でエンコードされているかを .meta から引く。
/// @note このエンジンは _SRGB フォーマットの SRV を作らず「シェーダーが自分で
/// @note SRGBToLinear する」規約で統一されているため、素材ごとに判定が要る
/// @note (手描きは sRGB、ProceduralVFXTextures はリニアで焼くため一律に決められない)。
[[nodiscard]] bool IsEffectTextureSrgb(const std::string& texturePath);
static_assert(sizeof(TrailCB) == 224, "TrailCB layout mismatch");

/// @brief Terrain シェーダーの b1。
/// @note LAYOUT: Assets/Shaders/Terrain/TerrainSurface.hlsli の TerrainCB と完全に一致させること。
/// @note 層ごとの値は StructuredBuffer (TerrainLayerGpu) へ移した。CB の固定長配列では層数に上限が残る。
/// @see Docs/design/terrain-layers.md
struct TerrainObjectCB {
    math::Matrix4 worldMatrix;
    math::Matrix4 wvpMatrix;
    /// @brief 天候 (x=wetness, y=darkening, z=puddleAmount)。
    /// @note 地形シェーダーは b1 を TerrainCB に使うため b8 を宣言できず、値はここで手渡す。
    math::Vector4 weather;
    /// @brief x=ローカル幅 X [m], y=ローカル奥行 Z [m], z=heightBlendDepth, w=自動ブレンドを持つ層があるか (0/1)。
    math::Vector4 terrainParams;
    std::uint32_t layerBufferIndex = 0xFFFFFFFFu; ///< @note TerrainLayerGpu 配列の bindless 添字
    std::uint32_t layerCount       = 0;
    std::uint32_t splatColumns     = 1;
    std::uint32_t splatRows        = 1;
};
static_assert(sizeof(TerrainObjectCB) == 176, "TerrainObjectCB size mismatch");

/// @brief 地形 1 層ぶんの GPU パラメーター (StructuredBuffer の 1 要素)。
/// @note LAYOUT: TerrainSurface.hlsli の TerrainLayer と一致させること。テクスチャ添字に無効値を入れない。
struct TerrainLayerGpu {
    std::uint32_t diffuseIndex     = 0;
    std::uint32_t normalIndex      = 0;
    std::uint32_t aoRoughnessIndex = 0;
    std::uint32_t heightIndex      = 0;
    float tilingX            = 8.0f;
    float tilingZ            = 8.0f;
    float normalStrength     = 1.0f;
    float roughness          = 0.8f;
    float ambientOcclusion   = 1.0f;
    float hasAoRoughness     = 0.0f;
    float hasHeight          = 0.0f;
    float heightBlend        = 0.0f;
    float autoMinHeight      = -10000.0f;
    float autoMaxHeight      = 10000.0f;
    float autoHeightFade     = 1.0f;
    float autoBlendEnabled   = 0.0f;
    float autoMinSlope       = 0.0f;
    float autoMaxSlope       = 1.0f;
    float autoSlopeFade      = 0.1f;
    float autoBlendStrength  = 1.0f;
    float triplanar          = 0.0f;
    float triplanarSharpness = 4.0f;
    float macroScale         = 0.1f;
    float macroStrength      = 0.0f;
};
static_assert(sizeof(TerrainLayerGpu) == 96, "TerrainLayerGpu size mismatch");

/// @note WaterVertex — Assets/Shaders/Water/Water.hlsl の WaterVSInput と一致する頂点。
struct WaterVertex {
    math::Vector3 position;
    math::Vector2 uv;
};
static_assert(sizeof(WaterVertex) == 20, "WaterVertex size mismatch");

/// @note WaterCB — Water シェーダーの b1。
/// @note LAYOUT: Assets/Shaders/Water/Water.hlsl の WaterCB と完全に一致させること。
struct WaterCB {
    math::Matrix4 worldMatrix;
    math::Matrix4 wvpMatrix;
    math::Vector4 shallowColorDepth;
    math::Vector4 deepColorDepth;
    math::Vector4 surfaceParams;
    math::Vector4 normalParams;
    math::Vector4 timeParams;
    math::Vector4 foamParams;
    math::Vector4 refractionFlowParams;
    math::Vector4 waveDir[4];
    math::Vector4 waveParams[4];
    math::Vector4 detailParams;
    math::Vector4 sssParams;
    math::Vector4 reflectParams;
    math::Vector4 flowParams;
    /// @note x=方向広がり [0,1]、y=有効な流れの本数、zw=描画ビューが求めた集中格子のローカル中心。
    math::Vector4 waveShapeParams;
    /// @note 流れの場が水面へ出す形。xy = 中心のワールド XZ [m], z = 影響半径 [m], w = 変位 [m] (符号つき)。
    /// @note 本数は waveShapeParams.y が持つ。
    math::Vector4 surfaceFlowA[8];
    /// @note x = 流速 [m/s] (Vortex は回る向きの符号つき), y = 距離減衰の指数, z = 型 (FlowFieldType),
    /// @note w = 質感へ回す強さ [0,1]。
    math::Vector4 surfaceFlowB[8];
    /// @note xy = Uniform の XZ 向き (正規化), z = 速度場アトラスのタイル番号 (-1 で無効),
    /// @note w = 焼いた値の復号係数 (maxMagnitude)。
    math::Vector4 surfaceFlowC[8];
    /// @note Baked の逆回転クォータニオン (x, y, z, w)。
    math::Vector4 surfaceFlowD[8];
    /// @note xyz = Baked の箱の半径 [m], w = 水面の基準面 Y − 場の中心 Y [m]。
    math::Vector4 surfaceFlowE[8];
    /// @note x=近景格子の有効フラグ、y=近景の頂点間隔 [m]、zw=元格子の X/Z 分割数。
    math::Vector4 gridParams;
};
/// @note Matrix4 x2 (128) + float4 x7 (112) + waveDir[4]/waveParams[4] (128) + float4 x5 (80)
/// @note + surfaceFlowA〜E[8] (640) + gridParams (16)
static_assert(sizeof(WaterCB) == 1104, "WaterCB size mismatch");

/// @note WaterEffectParams — Water.hlsl の MaterialConstants (b2) の C++ ミラー。
struct WaterEffectParams {
    float rimGlowStrength    = 0.40f;
    float minShallowAlpha    = 0.65f;
    float specularStrength   = 0.75f;
    /// @note 旧 specularExponent。ハイライトの鋭さは smoothness から導出するため廃止した枠。
    float _pad0              = 0.0f;
    float skyReflectTint[3]  = { 0.45f, 0.82f, 1.0f };
    /// @note 旧 envMapBlend。空反射の混合率は skyReflection が持つため廃止した枠。
    float _pad1              = 0.0f;
    float rippleRingColor[3] = { 0.88f, 0.97f, 1.0f };
    float rippleRingStrength = 0.72f;
};
static_assert(sizeof(WaterEffectParams) == 48, "WaterEffectParams layout mismatch with MaterialConstants");

struct RenderPassHandles {
    /// @note selectionMaskRT / outlineRT はここから外した。名前 "SelectionMask" / "Outline" を
    /// @note 申告したパスが res.Target() で引く。
    /// @note ここが «申告とは別» の第 2 の正本になると両者の食い違いに誰も気付けないため、
    /// @note 名前 1 本に寄せ切るまで引ける経路を順に閉じている。
    /// @note ランタイムの輪郭マスク (RGB=要求された色 / A=太さ)。エディタ選択のマスク
    /// @note («選ばれているか» の 1 ビット) とは別に持つ。
    renderer::ResourceHandle<renderer::RenderTargetTag> customPostProcessRT[2];

    /// @note Bloom のミップ連鎖と、各段の実寸。テクセルサイズを CB へ入れるのに要る。
    /// @note GetDimensions で引けなくはないが、書き込み先しか分からないため
    /// @note 「読み込み元のテクセルサイズ」は CPU 側から渡す必要がある。
    renderer::ResourceHandle<renderer::TextureTag> bloomChain[kBloomMipCount];
    /// @note 足し戻しの書き先。読みながら書けないので、ダウンサンプル用とは別に持つ。
    renderer::ResourceHandle<renderer::TextureTag> bloomUpChain[kBloomMipCount];
    uint32_t bloomChainWidth[kBloomMipCount]  = {};
    uint32_t bloomChainHeight[kBloomMipCount] = {};
    renderer::ResourceHandle<renderer::TextureTag> bloomHalf;   ///< @note = bloomChain[0]
    renderer::ResourceHandle<renderer::TextureTag> ssaoRaw;
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
    /// @note objectMaskShader のインスタンシング変種。
    /// @note 無効なら束ねずに 1 件ずつ出す (絵は変わらない)。
    /// @see Docs/design/gpu-instancing.md
    renderer::ResourceHandle<renderer::ShaderTag> objectMaskInstancedShader;
    renderer::ResourceHandle<renderer::ShaderTag> objectMaskSkinnedShader;
    /// @note 全画面コピー。読みながら書けない場所で «今の絵» を退避するのに使う。
    renderer::ResourceHandle<renderer::ShaderTag> copyColorShader;
    /// @note 縮小して走ったカスタムパスの結果を実寸へ戻す。
    renderer::ResourceHandle<renderer::ShaderTag> customComposeShader;
    renderer::ResourceHandle<renderer::ShaderTag> fxaaShader;
    /// @note 内部解像度の最終画を出力先の実寸へ引き伸ばす。等倍のフレームでは使わない。
    renderer::ResourceHandle<renderer::ShaderTag> upscaleShader;
    /// @note 内部解像度が出力より大きいとき (スーパーサンプリング) に、出力 1 画素の足跡を平均して縮める。
    renderer::ResourceHandle<renderer::ShaderTag> downscaleShader;
    std::vector<renderer::ResourceHandle<renderer::ShaderTag>> customPostProcessShaders;

    renderer::ResourceHandle<renderer::PipelineStateTag> selectionMaskPSO;
    renderer::ResourceHandle<renderer::PipelineStateTag> postprocPSO;
    renderer::ResourceHandle<renderer::PipelineStateTag> causticsPSO;

    renderer::ResourceHandle<renderer::ConstantBufferTag> frameCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> objectCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> lightCB;
    /// @note 最終フォールバックの単位行列パレット。スケルトンが解決できない場合のみ使う。
    /// @note 通常は Model::referencePoseCB (リファレンスポーズ) が優先される。
    /// @note ResolveSkinningCB() を必ず経由すること。
    renderer::ResourceHandle<renderer::ConstantBufferTag> bindPoseSkinningCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> postprocCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> outlineCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> objectMaskCB;

    /// @note 可視サーフェスのレイヤー番号 + 1 を持つ受信バッファ。レイヤーフィルタを持つ
    /// @note デカールが 1 つでもある フレームだけ描く。
    renderer::ResourceHandle<renderer::RenderTargetTag>   decalMaskRT;
    renderer::ResourceHandle<renderer::ShaderTag>         decalShader;
    renderer::ResourceHandle<renderer::ShaderTag>         decalMaskShader;
    renderer::ResourceHandle<renderer::ShaderTag>         decalMaskSkinnedShader;
    renderer::ResourceHandle<renderer::PipelineStateTag>  decalPSO;
    renderer::ResourceHandle<renderer::PipelineStateTag>  decalMaskPSO;
    renderer::ResourceHandle<renderer::ConstantBufferTag> decalCB;
    /// @note 組み込みシェーダー用の b2。.mat を持つデカールは Material 側の cbuffer を使う。
    renderer::ResourceHandle<renderer::ConstantBufferTag> decalMaterialCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> decalReceiverCB;

    renderer::ResourceHandle<renderer::ShaderTag>         shadowShader;
    renderer::ResourceHandle<renderer::ShaderTag>         shadowSkinnedShader;
    /// @note shadowShader のインスタンシング変種。world を b1 でなく VS の t0 から引く。
    /// @note 無効なら EmitShadowCasters は束ねずに 1 件ずつ出す (絵は変わらない)。
    /// @see Docs/design/gpu-instancing.md
    renderer::ResourceHandle<renderer::ShaderTag>         shadowInstancedShader;
    renderer::ResourceHandle<renderer::ConstantBufferTag> shadowCB;

    /// @name モーションベクター
    /// @{
    /// @note RG = 現 UV - 前フレーム UV、B = 書き込み済みフラグ。TAA / MotionBlur が t26 で読む。
    /// @note RGBA16F を使うのは CreateRenderTarget にフォーマット引数が無く RG16F を作れないため。
    renderer::ResourceHandle<renderer::ShaderTag>         velocityShader;
    /// @note velocityShader のインスタンシング変種。b1 の 2 枠目 (prevWorld) も per-instance で運ぶ。
    /// @note 無効なら束ねずに 1 件ずつ出す (絵は変わらない)。
    /// @see Docs/design/gpu-instancing.md
    renderer::ResourceHandle<renderer::ShaderTag>         velocityInstancedShader;
    renderer::ResourceHandle<renderer::ShaderTag>         velocitySkinnedShader;
    /// @}

    /// @name Spot / Point シャドウ
    /// @{
    /// @note Directional の CSM (shadowMapRT) とは別のアトラス。t28 へ束縛する。
    /// @note CSM のタイル数は視錐台の分割で、こちらは影付きライトの本数で決まる。
    /// @note 面積を奪い合わせると、ライトを 1 つ置いただけで遠景カスケードが粗くなる。
    renderer::ResourceHandle<renderer::ConstantBufferTag> punctualShadowCB;
    /// @}

    /// @name ライト Cookie (投影テクスチャ)
    /// @{
    /// @note 複数の Cookie を 1 枚へ敷き詰めたアトラス。t31 へ束縛する。
    /// @note 1 回の PS 呼び出しの中でまとめて評価するので、ライトごとに差し替えられない。
    renderer::ResourceHandle<renderer::ShaderTag>         cookieBlitShader;
    renderer::ResourceHandle<renderer::ConstantBufferTag> cookieBlitCB;
    renderer::ResourceHandle<renderer::PipelineStateTag>  cookieBlitPSO;
    /// @}

    /// @name 自動露出 (眼の順応)
    /// @{
    /// @note exposureHistogram は毎フレーム作り直す作業領域、exposureResult は
    /// @note 「順応済みの平均輝度」1 要素でフレームをまたいで生き続ける状態。
    /// @note どちらもビュー単位 (SceneView と GameView で順応の状態を共有すると、互いの明るさへ
    /// @note 引きずられて露出が振れる)。正本は RenderSystem の ViewRenderTargets。
    renderer::ResourceHandle<renderer::StructuredBufferTag> exposureHistogram;
    renderer::ResourceHandle<renderer::StructuredBufferTag> exposureResult;
    /// @note 最後に順応をリセットした世代。RequestAutoExposureReset() が全体の世代を進め、
    /// @note 各ビューは自分の値と食い違ったフレームで 1 度だけリセットする。0 = まだ一度も走っていない。
    /// @note この構造体はフレームごとに作り直されるので、正本は ViewRenderTargets が持つ。
    uint32_t exposureResetGeneration = 0;
    renderer::ResourceHandle<renderer::ShaderTag>           exposureHistogramCS;
    renderer::ResourceHandle<renderer::ShaderTag>           exposureAverageCS;
    renderer::ResourceHandle<renderer::ConstantBufferTag>   exposureCB;
    /// @}

    /// @name 体積雲の作業 RT (ビュー単位)
    /// @{
    /// @note 雲はビューの解像度で描くので、static で共有すると SceneView と GameView が
    /// @note 1 フレーム内に寸法を取り合い、毎フレーム作り直しになる。所有は ViewRenderTargets。
    renderer::SizedRenderTarget* cloudRT      = nullptr;
    renderer::SizedRenderTarget* cloudDepthRT = nullptr;
    /// @}

    /// @name 水面の屈折用コピー (ビュー単位)
    /// @{
    /// @note hdrRT を RTV として束縛したまま同じ色と深度を SRV で読めないので、水面を描く前に
    /// @note 別 RT へ写す。雲と同じ理由でビューが持つ (static だと 2 ビューで寸法を取り合う)。
    /// @note 水面が 1 つも見えないフレームでは確保しない。
    renderer::SizedRenderTarget* waterSceneColorRT = nullptr;
    renderer::SizedRenderTarget* waterSceneDepthRT = nullptr;
    /// @}

    /// @name パーティクル / コースティクスの作業 RT (ビュー単位)
    /// @{
    /// @note particleSceneColorRT は歪みパーティクルが屈折する «背景の退避»、
    /// @note particleOverdrawRT は重なり枚数の計数先、causticsDepthRT は深度のコピー。
    /// @note 3 つとも水面と同じ理由でビューが持つ。static のままだと SceneView と GameView が
    /// @note 1 枚を取り合い、寸法の違うフレームごとに作り直しが走る。
    renderer::SizedRenderTarget* particleSceneColorRT = nullptr;
    renderer::SizedRenderTarget* particleOverdrawRT   = nullptr;
    /// @note TAA の反応マスク。ParticleReactive パスが書き、TAA が t9 で読む。書いたフレームだけ valid。
    renderer::SizedRenderTarget* particleReactiveRT   = nullptr;
    bool                         particleReactiveValid = false;
    renderer::SizedRenderTarget* causticsDepthRT      = nullptr;
    /// @}

    /// @name フロクセル ボリューメトリック フォグ
    /// @{
    /// @note froxelScatter は散乱と消散の生値、froxelIntegrated は Z 積分後の
    /// @note 「加算する光 (rgb) と背景の透過率 (a)」。Composite は後者だけを読む。
    renderer::ResourceHandle<renderer::TextureTag>        froxelScatter;
    renderer::ResourceHandle<renderer::TextureTag>        froxelScatterHistory;
    renderer::ResourceHandle<renderer::TextureTag>        froxelIntegrated;
    renderer::ResourceHandle<renderer::ShaderTag>         froxelInjectCS;
    renderer::ResourceHandle<renderer::ShaderTag>         froxelIntegrateCS;
    renderer::ResourceHandle<renderer::ConstantBufferTag> froxelFogCB;

    /// @note コンピュートスキニング — ボーン変形を 1 フレーム 1 回だけ計算して各パスで共有する。
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

    /// @note 空連動 IBL (環境システム Phase A): SkyCapture の描画先キューブマップと、
    /// @note 6 面それぞれの view/projection を渡す b0 用 CB (カメラ frameCB とは別に持つ)。
    renderer::ResourceHandle<renderer::RenderTargetTag>   skyEnvCubeRT;
    renderer::ResourceHandle<renderer::ConstantBufferTag> skyCaptureFrameCB;

    renderer::ResourceHandle<renderer::ShaderTag>         particleShader;
    renderer::ResourceHandle<renderer::PipelineStateTag>  particlePSO;
    renderer::ResourceHandle<renderer::PipelineStateTag>  particleAlphaPSO;
    renderer::ResourceHandle<renderer::PipelineStateTag>  particlePremultipliedPSO;
    /// @note 頂点バッファは共有しない。エミッターごとに DynamicVertexBufferPool から借りる
    /// @note (共有 1 本だと DX12 で 2 個目以降の Update が 1 個目の Draw を壊す)。
    /// @note インデックスは全エミッター共通のクワッド列で、生成後は書き換えないので共有してよい。
    renderer::ResourceHandle<renderer::BufferTag>         particleIB;

    /// @note GPU パーティクル
    renderer::ResourceHandle<renderer::ShaderTag>         particleGpuSimCS;   ///< @note CS: シミュレーション+スポーン
    /// @note GPU ソート 3 段。半透明を大量に出すとき、描画順をカメラ距離で並べ替えるために使う。
    /// @note .cs.hlsl はエントリ 1 本なので、キー生成 / グローバル段 / LDS 段で 3 本に分かれる。
    renderer::ResourceHandle<renderer::ShaderTag>         particleGpuSortKeysCS;
    renderer::ResourceHandle<renderer::ShaderTag>         particleGpuSortStepCS;
    renderer::ResourceHandle<renderer::ShaderTag>         particleGpuSortLocalCS;
    /// @note メッシュパーティクルのインスタンス描画 (VS が SV_InstanceID で粒子を引く)。
    renderer::ResourceHandle<renderer::ShaderTag>         particleGpuMeshShader;
    /// @note 自己影: 光源から見た密度を積む専用 RT / シェーダー / 光源行列を入れた frame CB。
    /// @note 頂点展開ロジックを Particle.hlsl と共有するため b0 の view/viewProjection だけを
    /// @note 光源のものへ差し替える。h.frameCB を書き換えると後続パスへ漏れるため別 CB を持つ。
    renderer::ResourceHandle<renderer::RenderTargetTag>   particleSelfShadowRT;
    renderer::ResourceHandle<renderer::ShaderTag>         particleSelfShadowShader;
    renderer::ResourceHandle<renderer::ConstantBufferTag> particleSelfShadowFrameCB;
    /// @note 自己影の密度バッファ 1 辺の解像度 [px]。
    /// @note 拾うのは「煙の内部で光がどれだけ減るか」という低周波の情報で輪郭の鮮鋭さは
    /// @note 要らないため、シャドウマップより粗くしてフィルレートを抑える。
    static constexpr std::uint32_t kSelfShadowResolution = 512u;
    /// @note VS+PS: billboard 描画。合成モードは PSO 側だけで切り替えるためシェーダーは 1 本。
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
    /// @note gbufferShader のインスタンシング変種。
    /// @note 無効なら GBuffer パスは束ねずに 1 件ずつ出す (絵は変わらない)。
    /// @see Docs/design/gpu-instancing.md
    renderer::ResourceHandle<renderer::ShaderTag>         gbufferInstancedShader;
    /// @note gbufferShader のスキンド変種。b7 のパレットで VS が変形する。
    /// @note コンピュートスキニングが効いていれば使わない (変形済みの頂点を素の GBuffer で描く)。
    /// @see Docs/design/pipeline-boundary.md §3
    renderer::ResourceHandle<renderer::ShaderTag>         gbufferSkinnedShader;
    renderer::ResourceHandle<renderer::ShaderTag>         deferredLightingShader;
    renderer::ResourceHandle<renderer::ShaderTag>         depthCopyShader;
    /// @}

    /// @name クラスタライトカリング (Forward+ / Deferred+)
    /// @{
    /// @note punctualLightBuffer は PS の t29 / CS の t14 へ、clusterIndexBuffer は PS の t30 /
    /// @note CS の u2 へ束縛する。clusterCB (b9) はモードとグリッド係数を運ぶ。
    renderer::ResourceHandle<renderer::StructuredBufferTag> punctualLightBuffer;
    renderer::ResourceHandle<renderer::StructuredBufferTag> clusterIndexBuffer;
    renderer::ResourceHandle<renderer::ConstantBufferTag>   clusterCB;
    renderer::ResourceHandle<renderer::ShaderTag>           clusterCullCS;

    /// @note clusterCB と同じ内容で、供給モードだけ Linear に固定した版。
    /// @note クラスタリストはメインカメラの視錐台に対して 1 回だけ作られるので、別視点の
    /// @note パス (リフレクションプローブ) が引くとまったく別の場所のライトを拾う。
    renderer::ResourceHandle<renderer::ConstantBufferTag>   clusterLinearCB;
    /// @}

    /// @name Advanced Graphics
    /// @{

    /// @note AdvancedGraphics 共用定数バッファ (b8)
    renderer::ResourceHandle<renderer::ConstantBufferTag> advancedGraphicsCB;

    /// @note IBL (Image-Based Lighting)
    /// @note テクスチャハンドルは ResourceManager から取得した静的リソースで、
    /// @note シーンのスカイドームが変わるまで再ロード不要。
    renderer::ResourceHandle<renderer::TextureTag>        iblIrradiance;   ///< @note Diffuse irradiance cubemap
    renderer::ResourceHandle<renderer::TextureTag>        iblPrefilter;    ///< @note Specular prefiltered cubemap
    renderer::ResourceHandle<renderer::TextureTag>        iblBrdfLut;      ///< @note BRDF 積分 LUT (512x512 R16G16F)
    renderer::ResourceHandle<renderer::ShaderTag>         iblBrdfBakeShader; ///< @note CS: BRDF LUT をスタートアップ時に焼く
    renderer::ResourceHandle<renderer::RenderTargetTag>   iblBrdfLutRT;    ///< @note BRDF LUT bake 用 RT (静的)

    /// @note SSR (Screen Space Reflections)
    renderer::ResourceHandle<renderer::TextureTag>        ssrResult;       ///< @note SSR Compute 出力テクスチャ
    renderer::ResourceHandle<renderer::ShaderTag>         ssrShader;       ///< @note CS
    /// @note a=1 は重み付け済み鏡面間接光、a=2 はガラスの全放射輝度。a=0 は従来反射へ戻す。
    renderer::ResourceHandle<renderer::TextureTag>        rayReflectionResult;

    /// @note Volumetric Lighting
    renderer::ResourceHandle<renderer::TextureTag>        volumetricResult;
    renderer::ResourceHandle<renderer::ShaderTag>         volumetricShader;
    renderer::ResourceHandle<renderer::ShaderTag>         volumetricCloudShader;
    renderer::ResourceHandle<renderer::ShaderTag>         cloudUpscaleShader; ///< @note ハーフ解像度→HDR 合成
    renderer::ResourceHandle<renderer::PipelineStateTag>  volumetricCloudPSO;
    renderer::ResourceHandle<renderer::PipelineStateTag>  volumetricCloudPremultipliedPSO; ///< @note 雲の premultiplied 合成専用
    renderer::ResourceHandle<renderer::ConstantBufferTag> volumetricCloudCB;
    /// @note 3D ボリューメトリック雲ノイズ (起動時 CPU 焼き・タイラブル)。
    /// @note shape  = 128³ 低周波 Perlin-Worley + Worley FBM 帯 (RGBA)
    /// @note detail = 32³  高周波 Worley FBM (縁の侵食用)
    renderer::ResourceHandle<renderer::TextureTag>        cloudShapeTex;
    renderer::ResourceHandle<renderer::TextureTag>        cloudDetailTex;

    /// @note TAA (Temporal Anti-Aliasing)
    /// @note taaHistory は前フレームの TAA 出力を保持する永続 RT。解像度変更時のみ
    /// @note 再生成し、毎フレーム ping-pong で入れ替える。
    renderer::ResourceHandle<renderer::RenderTargetTag>   taaHistoryA;     ///< @note ping-pong バッファ A
    renderer::ResourceHandle<renderer::RenderTargetTag>   taaHistoryB;     ///< @note ping-pong バッファ B
    renderer::ResourceHandle<renderer::ShaderTag>         taaShader;       ///< @note VS+PS
    renderer::ResourceHandle<renderer::PipelineStateTag>  taaPSO;
    /// @note A→B→A... の ping-pong フラグ。この構造体はフレームごとに作り直されるので、
    /// @note 正本はビュー側 (RenderSystem の ViewRenderTargets) が持ち、ここへは複写して渡す。
    bool                                                  taaFlip = false;

    /// @note Motion Blur
    renderer::ResourceHandle<renderer::TextureTag>        motionBlurResult;
    renderer::ResourceHandle<renderer::ShaderTag>         motionBlurShader;

    /// @note GTAO (Ground Truth Ambient Occlusion)
    renderer::ResourceHandle<renderer::TextureTag>        gtaoRaw;
    renderer::ResourceHandle<renderer::ShaderTag>         gtaoShader;
    renderer::ResourceHandle<renderer::ShaderTag>         gtaoBlurShader;

    /// @note Contact Shadows
    renderer::ResourceHandle<renderer::ShaderTag>         contactShadowShader;

    /// @note Lens Flare
    renderer::ResourceHandle<renderer::ShaderTag>         lensFlareShader;
    renderer::ResourceHandle<renderer::PipelineStateTag>  lensFlarePSO;    ///< @note ADDITIVE ブレンド
    /// @note CPU生成32^3 RGBA8 LUT。外部DDSに依存せずCompositeのTexture3D(t22)へ束縛する。
    renderer::ResourceHandle<renderer::TextureTag>        proceduralColorLut;

    /// @name Light Probe Volume
    /// @{
    /// @note このフレームのライティングが引く SH ボリューム。[0] = 内側 (t22)、[1] = 外側 (t21)。無効ハンドルなら束縛しない。
    renderer::ResourceHandle<renderer::TextureTag>        lightProbeSH[2];
    /// @note 6 面を SH へ射影する CS と、その定数 (b0)。
    renderer::ResourceHandle<renderer::ShaderTag>         lightProbeProjectCS;
    renderer::ResourceHandle<renderer::ConstantBufferTag> lightProbeProjectCB;
    /// @note 壁に埋まったプローブを周りで埋める CS と、その定数 (b0)。
    renderer::ResourceHandle<renderer::ShaderTag>         lightProbeDilateCS;
    renderer::ResourceHandle<renderer::ConstantBufferTag> lightProbeDilateCB;
    /// @note 面の表裏だけを書くシェーダー (カリング無し PSO で描く)。
    renderer::ResourceHandle<renderer::ShaderTag>         lightProbeFacingShader;
    /// @note 捕捉の面ごとの b0。カメラの frameCB を上書きしないために分ける。
    renderer::ResourceHandle<renderer::ConstantBufferTag> lightProbeCaptureFrameCB;
    /// @note 捕捉で描くマテリアルが読む b8。画面空間 AO など «メインカメラの画面» に依存する項を切った写し。
    renderer::ResourceHandle<renderer::ConstantBufferTag> lightProbeCaptureAdvancedCB;
    /// @}
    /// @}
    /// @note View-owned normalized GBuffer draw snapshots; numeric handles from a different ResourceManager may collide.
    renderer::ResourceHandle<renderer::ConstantBufferTag> gbufferMaterialCB;
    /// @note ReflectionProbe 捕捉専用。主ビューの b4/b12 を変更せず、camera-dependent shadow/cookie は unavailable。
    renderer::ResourceHandle<renderer::ConstantBufferTag> reflectionProbeCaptureShadowCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> reflectionProbeCapturePunctualCB;
};

/// @note ShadowCascade — カスケード 1 枚ぶんの描画情報。RenderSystem が毎フレーム組み立て、
/// @note ShadowPass がアトラスのタイルへ描き、各ライティングパスが CB へ転送する。
/// @note 行列・カリング錐台・書き込み先タイル・バイアスを束ねてあるので、ShadowPass 側は
/// @note 「タイルを選んで既存の caster 提出を回す」だけで済む。
struct ShadowCascade {
    math::Matrix4 viewProjection;
    /// @note ライトビュー単体。第 3 行がライト前方への射影なので、caster を光源に近い順へ
    /// @note 並べ替えるための深度キー算出に使う (Hi-Z を効かせるための描画順)。
    math::Matrix4 view;
    /// @note ライト視点のワールド位置。
    math::Vector3 eyePos;
    /// @note このカスケードの caster カリング用錐台 (viewProjection から抽出済み)。
    math::Frustum frustum;
    /// @note アトラス上の位置。xy = UV オフセット, zw = UV スケール (HLSL cascadeAtlasRect と同値)。
    math::Vector4 atlasRect;
    /// @note アトラス上のピクセル矩形。ShadowPass が IRenderer::SetViewport へ渡す。
    uint32_t      viewportX    = 0;
    uint32_t      viewportY    = 0;
    uint32_t      viewportSize = 0;
    /// @note このカスケードの正射影深度レンジで正規化した NDC バイアス。
    float         biasNDC = 0.0f;
    /// @note このカスケードの 1 テクセルが覆うワールド距離 [m]。caster の極小カリングに使う。
    float         texelWorldSize = 0.0f;
    float         splitFar = 0.0f;
};

/// @note PunctualShadowView — Spot / Point シャドウアトラスのタイル 1 枚ぶんの描画情報。
/// @note RenderSystem が毎フレーム組み立て、ShadowPass がタイルへ描き、
/// @note GeometryPassHelpers が PunctualShadowConstantsCB へ転送する。
/// @note ShadowCascade と別の型なのは、あちらが正射影でワールド距離が一意に決まるのに対し
/// @note こちらは透視投影で深度によって変わるため。同じ型だと使ってよい欄が分からなくなる。
struct PunctualShadowView {
    math::Matrix4 viewProjection;
    /// @note ライトビュー単体。caster を光源に近い順へ並べるための深度キー算出に使う。
    math::Matrix4 view;
    /// @note ライト視点のワールド位置 (= ライトの位置)。
    math::Vector3 eyePos;
    /// @note caster カリング用錐台 (viewProjection から抽出済み)。
    math::Frustum frustum;
    /// @note アトラス上の位置。xy = UV オフセット, zw = UV スケール。
    math::Vector4 atlasRect;
    /// @note アトラス上のピクセル矩形。ShadowPass が IRenderer::SetViewport へ渡す。
    uint32_t      viewportX    = 0;
    uint32_t      viewportY    = 0;
    uint32_t      viewportSize = 0;
    /// @note 透視投影の深度レンジで正規化した NDC バイアスと影の濃さ。
    float         biasNDC        = 0.0f;
    float         shadowStrength = 1.0f;
    /// @note 光源半径ぶんの半影の広がり [テクセル]。0 で従来どおり硬い縁。
    float         penumbraTexels = 0.0f;
    /// @note 1 テクセルが張る角度 [rad] = 2*tan(halfFov) / タイル一辺。
    /// @note 透視投影ではテクセルの覆うワールド距離が深度に比例するので固定値を持てない。
    /// @note 角度なら深度に依存せず、caster の「半径 / 距離」と直接比べられる。
    float         texelAngularSize = 0.0f;
};

/// @note LightCookieView — Cookie アトラスのタイル 1 枚。
/// @note シャドウのスロットとは独立に割り当てる。影を落とさないライトにも Cookie は付けられる。
struct LightCookieView {
    math::Matrix4 viewProjection;
    /// @note アトラス上の位置。xy = UV オフセット, zw = UV スケール。
    math::Vector4 atlasRect;
    /// @note アトラス上のピクセル矩形の左上。サイズは kLightCookieTileSize 固定。
    uint32_t      viewportX = 0;
    uint32_t      viewportY = 0;
    /// @note 焼き直しの要否を判定するキー。前フレームと同じならタイルをそのまま使う。
    std::string   sourcePath;
    renderer::ResourceHandle<renderer::TextureTag> sourceTexture;
    bool srgb = false;
    float         rotationRad = 0.0f;
};

/// @note UI 合成パスの設定。実体は RenderSystem.hpp 側 (呼び出し元の Viewport が持つ)。


/// @note グラフの外から引くときの «申告» (空)。申告チェックを素通しにするために使う。
inline const std::vector<renderer::RenderGraph::ResourceAccess> kOutsideGraphAccesses{};

}
