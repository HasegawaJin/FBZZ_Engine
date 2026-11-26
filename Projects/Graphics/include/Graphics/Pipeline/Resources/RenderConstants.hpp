/// @file    RenderConstants.hpp
/// @brief   描画パスからシェーダーへ渡す定数、頂点、GPU リソースの参照。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once
#include <Graphics/Renderer/Commands/ShaderBindingSlots.hpp>
#include <Graphics/Api.hpp>
#include <Graphics/Pipeline/Passes/RenderPassInjectionPoint.hpp>

#include <Graphics/Renderer/View/Camera.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/View/Lighting/LightSystem.hpp>
#include <Graphics/RenderGraph/RenderGraph.hpp>
#include <Graphics/Renderer/View/RenderSettings.hpp>
#include <Graphics/Renderer/Resources/ResourceHandle.hpp>
#include <Graphics/Renderer/Resources/ResourceManager.hpp>
#include <Graphics/Renderer/View/Lighting/RenderEnvironment.hpp>
#include <Graphics/Pipeline/Resources/EnvironmentResources.hpp>
#include <Graphics/Pipeline/Resources/PassResources.hpp>
#include <Graphics/Pipeline/Culling/OcclusionCuller.hpp>
#include <Math/Geometry/Frustum.hpp>
#include <Math/LinearAlgebra/Matrix4.hpp>
#include <Math/LinearAlgebra/Vector2.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <Math/LinearAlgebra/Vector4.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>


namespace fbzz::renderer { struct RenderScene; }

namespace fbzz::renderer {


struct RenderPassContext;



/// @note GPU パーティクル CS が参照する流れ 1 本分 (96 bytes)。
/// @note LAYOUT: ParticleGpuSim.cs.hlsl の GpuFlowField と完全に一致させること。
/// @note 定数バッファでなく StructuredBuffer にするのは、cbuffer は固定長で
/// @note あふれた流れを黙って捨てるしかないため。SRV なら上限が消え、焼いた場のため
/// @note 太らせても b0 のオフセット (末尾追加のみが規約) は動かない。
struct GpuFlowField {
    math::Vector4 posRadius;     ///< xyz=ワールド位置, w=影響半径 (<=0 で無限)
    math::Vector4 dirStrength;   ///< xyz=流向/渦軸 (ワールド・正規化済み), w=流速 [m/s]
    math::Vector4 params;        ///< x=種類(FlowFieldType), y=falloffPower,
                                 /// @note z=noiseFrequency, w=noiseSpeed
    /// @name Baked 型のときだけ使う
    /// @{
    math::Vector4 fieldRotation; ///< ワールド → 場のローカルへ戻す逆回転 (xyzw = クォータニオン)
    math::Vector4 fieldExtents;  ///< xyz=ワールド半径 [m], w=予約 (旧 tightness)
    math::Vector4 fieldTile;     ///< x=アトラスのタイル番号 (<0 で無効), y=maxMagnitude, zw=予約
    /// @}
};

static_assert(sizeof(GpuFlowField) == 96,
    "GpuFlowField must match ParticleGpuSim.cs.hlsl (96 bytes)");

/// @note GPU パーティクル CS 用定数バッファ (b0) — 688 bytes, 16-byte aligned
struct GpuParticleEmitterCB {
    math::Vector3 emitterPos; ///< 粒子を放出するエミッターのワールド位置。
    float         deltaTime; ///< この粒子シミュレーションを進める秒数。
    math::Vector3 gravity;        ///< ParticleEmitterSettings::gravity [m/s^2]
    uint32_t      maxParticles; ///< GPU 粒子プールで処理できる粒子数の上限。
    math::Vector4 colorStart; ///< 粒子の生成直後に適用する色。
    math::Vector4 colorEnd; ///< 粒子の寿命の終端に適用する色。
    uint32_t      spawnCount;       ///< 今フレームのスポーン数
    uint32_t      spawnOffset;      ///< リングバッファ書き込み先頭インデックス
    float         colorCurvePower;  ///< CPU と一致: pow(t, colorCurvePower)
    /// @note 旧 velocityDamping と同じ枠・同じ単位 [1/s]。流れへ寄る速さになった。
    float         flowCoupling;
    float         sizeStart; ///< 粒子生成時の大きさ。
    float         sizeEnd; ///< 粒子の寿命の終端での大きさ。
    float         sizeCurvePower; ///< 開始サイズと終了サイズの補間に使う指数。
    float         pad0; ///< HLSL 側の定数配置に合わせて確保する予約領域。
    uint32_t      spriteColumns; ///< フリップブックを横に分割する列数。
    uint32_t      spriteRows; ///< フリップブックを縦に分割する行数。
    uint32_t      spriteStartFrame; ///< スプライトシート上で再生を始めるコマ番号。
    uint32_t      spriteEndFrame; ///< スプライトシート上で再生を終えるコマ番号。
    /// @name ノイズモジュール + 流れ (末尾追加で既存オフセットを変えない)
    /// @{
    float         time;             ///< カールノイズのスクロールに使う経過時間
    float         noiseStrength;    ///< エミッター固有乱流の強さ (0 で無効)
    float         noiseFrequency; ///< 粒子の移動を乱すノイズの空間周波数。
    float         noiseSpeed; ///< 粒子の移動を乱すノイズ場が時間とともに変化する速さ。
    uint32_t      flowFieldCount;   ///< gParticleForces (SRV) の有効本数
    uint32_t      flipbookMode; ///< 粒子の経過時間から表示コマを選ぶ方式。
    float         flipbookFramesPerSecond; ///< 時間指定で再生する場合の毎秒のコマ数。
    float         pad1; ///< HLSL 側の定数配置に合わせて確保する予約領域。
    /// @note 流れは StructuredBuffer へ移った (2026-09-11)。枠は詰めず空けたまま残す。
    /// @note この配列は b0 の途中にあり、消すと後続の全オフセットがずれて HLSL 側の
    /// @note gViewProjection 以降が丸ごと別の値を読む。CB は «末尾追加のみ» が規約なので、
    /// @note 384 バイトの無駄よりレイアウトが動かないことを優先する。
    math::Vector4 reservedFlowFields[24];
    math::Vector4 curveFlags;       ///< x=size, y=velocity, z=gradient, w=frameBlend
    math::Vector4 sizeCurveKeys01;  ///< time0,value0,time1,value1
    math::Vector4 sizeCurveKeys23; ///< 大きさの変化カーブ。キー 2 と 3 の時刻・値を順に格納する。
    math::Vector4 velocityCurveKeys01; ///< 速度の変化カーブ。キー 0 と 1 の時刻・値を順に格納する。
    math::Vector4 velocityCurveKeys23; ///< 速度の変化カーブ。キー 2 と 3 の時刻・値を順に格納する。
    math::Vector4 gradientTimes; ///< 粒子色のグラデーションの先頭 4 キーの時刻。
    math::Vector4 gradientColors[4]; ///< 先頭 4 キーの色。gradientTimes と同じ順で格納する。
    math::Matrix4 viewProjection; ///< ワールド座標をこのビューのクリップ座標に変換する行列。
    float         screenWidth; ///< 深度衝突で参照する画面の幅（画素）。
    float         screenHeight; ///< 深度衝突で参照する画面の高さ（画素）。
    float         depthThickness; ///< 深度画像の面と粒子の衝突を認める厚み。
    float         depthBounciness; ///< 深度衝突で反射した粒子の跳ね返りの強さ。
    uint32_t      depthCollision; ///< シーンの深度を粒子の衝突判定に使うか。
    uint32_t      depthResponse; ///< 深度で衝突を検出した後の粒子の応答方式。
    float         depthDamping; ///< 深度衝突時に粒子の速さを減衰させる量。
    float         depthPad; ///< HLSL 側の定数配置に合わせて確保する予約領域。
    /// @}
    /// @name over-lifetime モジュール追加分 (末尾追加で既存オフセットを変えない)
    /// @{
    /// @note 既存 curveFlags が埋まっているため 2 本目のフラグ束を持つ。
    math::Vector4 curveFlags2;          ///< x=rotation, y=drag, z/w=予約
    math::Vector4 rotationCurveKeys01;  ///< time0,value0,time1,value1
    math::Vector4 rotationCurveKeys23; ///< 回転の変化カーブ。キー 2 と 3 の時刻・値を順に格納する。
    math::Vector4 dragCurveKeys01; ///< 抵抗の変化カーブ。キー 0 と 1 の時刻・値を順に格納する。
    math::Vector4 dragCurveKeys23; ///< 抵抗の変化カーブ。キー 2 と 3 の時刻・値を順に格納する。
    math::Vector3 orbitalAxis;          ///< 正規化済み
    float         orbitalVelocity; ///< 軌道軸の周囲で粒子を回転させる速度。
    float         radialVelocity; ///< エミッター中心から半径方向に加える速度。
    /// @note bit0 = spriteRandomStartFrame / bit1 = spriteRandomRow
    uint32_t      spriteRandomFlags;
    float         velocityPad0; ///< HLSL 側の定数配置に合わせて確保する予約領域。
    float         velocityPad1; ///< HLSL 側の定数配置に合わせて確保する予約領域。
    /// @}
    /// @name カーブ 8 キー化の追加分
    /// @{
    /// @note 既存の *Keys01/23 はオフセットを変えずキー 4〜7 を末尾へ足す。
    /// @note CB は「末尾追加のみ」が規約 (途中へ挿すと HLSL 側の全オフセットがずれる)。
    math::Vector4 sizeCurveKeys45; ///< 大きさの変化カーブ。キー 4 と 5 の時刻・値を順に格納する。
    math::Vector4 sizeCurveKeys67; ///< 大きさの変化カーブ。キー 6 と 7 の時刻・値を順に格納する。
    math::Vector4 velocityCurveKeys45; ///< 速度の変化カーブ。キー 4 と 5 の時刻・値を順に格納する。
    math::Vector4 velocityCurveKeys67; ///< 速度の変化カーブ。キー 6 と 7 の時刻・値を順に格納する。
    math::Vector4 rotationCurveKeys45; ///< 回転の変化カーブ。キー 4 と 5 の時刻・値を順に格納する。
    math::Vector4 rotationCurveKeys67; ///< 回転の変化カーブ。キー 6 と 7 の時刻・値を順に格納する。
    math::Vector4 dragCurveKeys45; ///< 抵抗の変化カーブ。キー 4 と 5 の時刻・値を順に格納する。
    math::Vector4 dragCurveKeys67; ///< 抵抗の変化カーブ。キー 6 と 7 の時刻・値を順に格納する。
    math::Vector4 gradientTimes47; ///< 粒子色のグラデーションのキー 4～7 の時刻。
    math::Vector4 gradientColors47[4]; ///< 粒子色のグラデーションのキー 4～7 の色。
    /// @note 実キー数。4 キー固定だった頃は不要だったが、8 キー化で「どこまでが有効か」を
    /// @note GPU 側も知らないと、末尾のダミーキーを踏んで CPU と違う値を返す。
    math::Vector4 curveKeyCounts;   ///< x=size, y=velocity, z=rotation, w=drag
    /// @note 補間モード (0=Linear, 1=Step, 2=Smooth)。CPU の ApplyCurveInterpolation と対。
    math::Vector4 curveModes;       ///< x=size, y=velocity, z=rotation, w=drag
    math::Vector4 gradientMeta;     ///< x=キー数, y=補間モード, z/w=予約
    /// @}
};
/// @note 内訳: 従来 784 に over-lifetime 追加 112 (float4×5=80、orbitalAxis 12+orbitalVelocity 4=16、radialVelocity 4
/// @note spriteRandomFlags 4、pad 4×2=16) とカーブ 8 キー化 256 (float4×16: curve 4 本×2、gradient 1、色 4、meta 3) を足す。
static_assert(sizeof(GpuParticleEmitterCB) == 1152,
    "GpuParticleEmitterCB must match GpuEmitterCB in ParticleGpuSim.cs.hlsl (1152 bytes)");

struct PerFrameCB {
    math::Matrix4 view; ///< ワールド座標をカメラのビュー空間に変換する行列。
    math::Matrix4 projection; ///< ビュー空間からクリップ空間に変換するカメラの射影行列。
    math::Matrix4 viewProjection; ///< ワールド座標をこのビューのクリップ座標に変換する行列。
    math::Matrix4 invViewProjection; ///< 深度と画面上の位置からワールド座標を復元する逆ビュー射影行列。
    math::Vector3 cameraPos; ///< ビューを描画するカメラのワールド座標。
    float         nearZ; ///< カメラの近クリップ距離。
    float         farZ; ///< カメラの遠クリップ距離。
    /// @note WaterRenderPass だけが waterSsrEnabled として使う枠。他パスは 0 のまま。
    float         _reserved;
    /// @note 平行投影なら 1、遠近投影なら 0。
    /// @note カメラ視点の projection / viewProjection は Reversed-Z (near → 1、far → 0)。
    /// @note 深度バッファの値と視空間 Z の関係が射影で変わるため、Space.hlsli の
    /// @note LinearizeDepthAuto がこれで式を切り替えないと SSAO/SSR/コンタクトシャドウ/
    /// @note ソフトパーティクル/デカールが正投影ビューで一斉に破綻する。
    float         isOrthographic;
    /// @note Directional Auto Bake の影では、風変形前の葉の形状を使う。
    float         stableLeafShadow;
};

/// @note TAA サブピクセルジッターを織り込んだ GPU 用射影行列 (Reversed-Z) を返す。
/// @param[in] jitterNdcX,jitterNdcY NDC 単位のジッター量。TAA 非有効時は 0 を渡す。
/// @note ジッターはラスタライズする行列にだけ乗せる。カリング用の錐台には載せないこと
/// @note (半ピクセルのために可視判定を揺らす意味がない)。
/// @note 深度は near → 1、far → 0。カメラ視点の深度バッファ (reversedZ な RT) と対で使う。
inline math::Matrix4 MakeJitteredProjection(const renderer::Camera& camera,
                                            float jitterNdcX, float jitterNdcY)
{
    math::Matrix4 projection = camera.GetGpuProjectionMatrix();
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

/// @note b2 は TAA 専用。位相は連続した資源フレーム番号の剰余で、ビューのジッター位置と固定対応する。
struct alignas(16) TaaCycleCB {
    uint32_t phaseIndex = 0; ///< 現在のフレームに対応する TAA サンプリング位相。
    uint32_t enabled = 0; ///< 位相ごとの色履歴を使うときに立てるフラグ。
    uint32_t reserved[2]{}; ///< HLSL 側の定数配置に合わせて確保する予約領域。
};
static_assert(sizeof(TaaCycleCB) == 16);

struct PerObjectCB {
    math::Matrix4 world; ///< 頂点のローカル座標をワールド座標に変換する行列。
    math::Matrix4 worldInvTranspose; ///< 非一様スケールを含むワールド変換に合わせて法線を変換する逆転置行列。
    /// @note x = LOD ディザのしきい値。既定 0 は「遷移していない」= 全画素を描く。
    /// @note y = カードのワイヤー診断表示。0=無効、1=Lit、2=Unlit。
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
inline constexpr uint32_t kMaxPunctualLights = 1024;
inline constexpr uint32_t MANY_LIGHT_POOL_SIZE = 32;

/// @note ライト供給モード。ClusterConstants.hlsli の FBZZ_LIGHT_MODE_* と一致させること。
/// @note cbuffer 未束縛のパスは中身が全ゼロで読まれるため 0 = Legacy にしておくと、
/// @note b9 と t29/t30 を渡していない既存パスは 1 ビットも変わらず動く。
enum class ClusterLightMode : uint32_t {
    Legacy    = 0, ///< b3 の固定長 cbuffer (点 8 / スポット 4)
    Linear    = 1, ///< StructuredBuffer を全数走査 (カリング無効・A/B 検証用)
    Clustered = 2, ///< クラスタが持つライトだけ走査
    SAMPLED   = 3, ///< 重要度に基づく固定プールから確率補正付きで評価する。
};

[[nodiscard]] constexpr bool UsesClusterLightIndices(ClusterLightMode mode)
{
    return mode == ClusterLightMode::Clustered || mode == ClusterLightMode::SAMPLED;
}

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
    math::Vector3 position;    ///< 光源中心のワールド座標。
    float range;               ///< 光が届く範囲。クラスター選別と距離減衰に使う。
    math::Vector3 color;       ///< 照明計算に使う RGB 光源色。
    float intensity;           ///< 光源色に適用する強度。
    math::Vector3 direction;   ///< スポットや指向性を持つ面光源が向く方向。
    float innerCos;            ///< スポットが最大強度になる内側半角の余弦。
    float outerCos;            ///< スポットの寄与が消える外側半角の余弦。
    uint32_t type;             ///< シェーダーが光源形状を選ぶ PunctualLightType の値。
    int32_t shadowIndex = -1;  ///< 影の投影を引く添字。-1 は影を参照しない。
    int32_t cookieIndex = -1;  ///< クッキーの投影を引く添字。-1 はクッキーを参照しない。
    math::Vector3 tangent;     ///< 面の幅方向、またはチューブの長手方向のワールド軸。
    float halfWidth;           ///< 面の半幅、球・チューブでは半径。
    math::Vector3 bitangent;   ///< 面光源の高さ方向のワールド軸。
    float halfHeight;          ///< 面の半高、チューブでは軸方向の半長。
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

    math::Vector4 _punctualShadowPad0{}; ///< HLSL 側の定数配置に合わせて確保する予約領域。
};
static_assert(sizeof(PunctualShadowConstantsCB) == 2800,
    "PunctualShadowConstantsCB must match PunctualShadowConstants in "
    "Common/PunctualShadowConstants.hlsli (2800 bytes)");

/// @note FroxelFogCB — FroxelFogConstants.hlsli の FroxelFogConstants (b13) と一致させること。
struct FroxelFogCB {
    math::Matrix4 invViewProj; ///< フロクセルの位置をワールド空間に復元する逆ビュー射影行列。

    math::Vector3 cameraPos; ///< フロクセル格子を構築するカメラのワールド位置。
    float nearDistance = 0.1f; ///< フロクセル格子の手前端までのビュー空間距離（m）。
    math::Vector3 albedo; ///< 霧の散乱色。RGB ごとの成分。
    float farDistance = 64.0f; ///< フロクセル格子の奥端までのビュー空間距離（m）。
    math::Vector3 emissive; ///< 霧が自ら加える放射光の RGB 成分。
    float density = 0.02f; ///< 一様な媒体の消散係数（1/m）。

    float    anisotropy    = 0.4f; ///< Henyey–Greenstein 位相関数の g。正値は前方散乱。
    float    heightFalloff = 0.0f; ///< 基準高度を超えたときの密度の減衰係数（1/m）。
    float    heightStart   = 0.0f; ///< 密度の高度減衰を計算する基準のワールド Y 座標。
    /// @note スライス内のサンプル位置ずらし [0,1)。グリッドが粗いことによる
    /// @note 「霧の中の板」をフレーム間のちらつきへ散らすためのディザ。
    /// @note 単独で使うとちらつきがそのまま残る。必ず historyBlend の蓄積と対で使う。
    float    jitter        = 0.0f;

    uint32_t gridX = 0; ///< フロクセル格子の横方向のセル数。
    uint32_t gridY = 0; ///< フロクセル格子の縦方向のセル数。
    uint32_t gridZ = 0; ///< ビュー深度方向のスライス数。
    float    ambient = 1.0f; ///< 霧の散乱光に加える環境光の寄与。

    /// @note 前フレームのビュー射影。フロクセルのワールド座標を前フレームのグリッドへ
    /// @note 投影し直して履歴を引くのに使う (カメラが動いても履歴が付いてくる)。
    math::Matrix4 prevViewProj;
    /// @note 今フレームの寄与率。0 で履歴のみ、1 で蓄積なし (= 生のちらつき)。
    float    historyBlend = 1.0f;
    /// @note 履歴が使えないフレーム (初回 / 解像度変更 / 霧の再有効化) の印。
    uint32_t historyValid = 0;
    float    lightScattering = 1.0f; ///< 光源からの光を霧の散乱に反映する倍率。
    float    _pad         = 0.0f; ///< HLSL 側の定数配置に合わせて確保する予約領域。
};
static_assert(sizeof(FroxelFogCB) == 224,
    "FroxelFogCB must match FroxelFogConstants in Common/FroxelFogConstants.hlsli (224 bytes)");

/// @note フロクセル霧がフレームをまたいで持ち越す状態。実体はビュー単位で確保する。
/// @note グリッドは視錐台に貼り付くので、共有すると互いのボリュームを上書きし合い、
/// @note 相手のカメラ行列で履歴を引き直して霧が明滅する (prevViewProjection と同じ理由)。
struct FroxelFogViewState {
    /// @note 履歴の引き直しに使う前フレームのビュー射影。
    math::Matrix4 prevViewProjection = math::Matrix4::Identity(); ///< 現フレームのワールド位置を履歴画像上の位置へ投影する前フレームの行列。
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
    float    minEV        = -6.0f; ///< 輝度ヒストグラムがカバーする露出値の下端。
    float    evRange      = 20.0f; ///< ヒストグラムがカバーする露出値の幅。
    float    lowPercent   = 0.45f; ///< 露出計算で考慮する暗部側のパーセンタイル。
    float    highPercent  = 0.95f; ///< 露出計算で考慮する明部側のパーセンタイル。

    float    speedUp      = 3.0f; ///< 自動露出の上昇側の順応速度。
    float    speedDown    = 1.0f; ///< 自動露出の下降側の順応速度。
    float    deltaTime    = 0.0f; ///< 順応量を決めるフレームの経過時間（秒）。
    float    compensation = 0.0f; ///< 自動計算した露出に加える補正（段）。

    float    minEVClamp   = -8.0f; ///< 自動露出に許容する最小の露出値。
    float    maxEVClamp   =  8.0f; ///< 自動露出に許容する最大の露出値。
    /// @note 1 = 順応を飛ばして即座に合わせる。初回フレームとシーン切り替えで立てる。
    uint32_t reset        = 1;
    float    _pad0        = 0.0f; ///< HLSL 側の定数配置に合わせて確保する予約領域。
};
static_assert(sizeof(AutoExposureCB) == 48, "AutoExposureCB size mismatch");

/// @note CookieBlitCB — CookieBlit.hlsl の CookieBlitConstants (b2) と一致させること。
struct CookieBlitCB {
    float    cookieRotation = 0.0f;  ///< [rad]
    uint32_t cookieSrgb     = 0;     ///< 1 = 元テクスチャが sRGB エンコード
    float    _cookiePad[2]  = { 0.0f, 0.0f }; ///< HLSL 側の定数配置に合わせて確保する予約領域。
};
static_assert(sizeof(CookieBlitCB) == 16, "CookieBlitCB size mismatch");

struct ClusterConstantsCB {
    /// @note 1 クラスタが覆う画面上のピクセル数 = screenSize / (GridX, GridY)。
    /// @note CPU 側で割るのは、マテリアルパスが PostProcConstants (b5) を束縛せず
    /// @note シェーダー側で screenSize を参照できないため。
    float    clusterTilePx[2];
    float    clusterSliceScale;
    float    clusterSliceBias;
    uint32_t clusterLightMode;   ///< ClusterLightMode
    uint32_t punctualLightCount;
    uint32_t clusterDebugMode;   ///< 0=通常, 1=クラスタあたりライト数のヒートマップ
    /// @note Low six bits are the 1..32 evaluation budget; upper bits seed temporal sampling. Zero retains exact lighting.
    uint32_t manyLightControl = 0;
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

    float         shadowMapTexelSize[2]; ///< 1.0 / アトラス全体の解像度
    float         shadowBias;            ///< 単一カスケード時のバイアス (= cascadeBias.x)
    float         shadowStrength;        ///< 0=影なし, 1=完全な影

    int           shadowPcfRadius;  ///< PCF カーネル半径: 0=ハード, 1=3x3, 2=5x5, 3=7x7
    int           cascadeCount;     ///< 1 = 単一シャドウマップ (従来), 2〜4 = CSM
    float         cascadeBlend;     ///< カスケード境界のクロスフェード幅 [0,1]
    int           cascadeDebugView; ///< 1 = カスケード番号を色で可視化

    /// @note 雲シャドウ (Phase C)
    float         cloudShadowStrength; ///< 0=無効
    float         cloudShadowCoverage;
    float         cloudShadowScale;
    float         cloudShadowSpeed;

    float         cloudShadowTime;
    float         cloudShadowWindX;
    float         cloudShadowWindZ;
    float         _shadowPad0 = 0.0f; ///< HLSL 側の定数配置に合わせて確保する予約領域。
    /// @note CSM の選択はライト空間の重なりでなく、このビューの前方距離で行う。
    math::Vector4 cascadeSplitFar;
    math::Vector4 shadowCameraPosition;
    math::Vector4 shadowCameraForward;
};
static_assert(sizeof(ShadowConstantsCB) == 512,
    "ShadowConstantsCB must match ShadowConstants in Common/ShadowConstants.hlsli (512 bytes)");

struct AtmosphereCB {
    float rayleighScattering[3];
    float mieScattering; ///< 大気中の粒子によるミー散乱の係数。
    float planetRadius; ///< 大気の計算で地表とする球の半径。
    float atmosphereRadius; ///< 大気の外縁までを表す球の半径。
    float sunIntensity;
    float mieG; ///< ミー散乱の方向性を指定する位相関数の g。
    /// @name 月 (Phase B) ── HLSL AtmosphereConstants と一致させること (末尾追加・16byte 整列)。
    /// @{
    float moonEnabled;     ///< 0/1
    float moonSize;
    float moonBrightness;
    float starBrightness;
    float moonColor[3];
    float _moonPad1; ///< HLSL 側の定数配置に合わせて確保する予約領域。
    /// @}
};
static_assert(sizeof(AtmosphereCB) == 64);

/// @brief Light Probe Volume 1 つぶんの配置。intensity <= 0 で無効。
/// @note LAYOUT: AdvancedGraphicsConstants.hlsli の ProbeVolumeParams と一致させること (48 bytes)。
struct ProbeVolumeParamsCB {
    math::Vector3 boxMin{};                     ///< 箱の最小角 [world]
    float         intensity = 0.0f;             ///< 拡散 GI の倍率
    math::Vector3 invSize{ 1.0f, 1.0f, 1.0f };  ///< 1 / 箱の大きさ [1/m]
    float         fade = 0.0f;                  ///< 箱の縁で外側へ戻していく幅 [m]
    uint32_t      grid[3] = { 1u, 1u, 1u };
    float         normalBias = 0.0f;            ///< 法線方向へずらして引く距離 [m]
};
static_assert(sizeof(ProbeVolumeParamsCB) == 48, "ProbeVolumeParamsCB must match ProbeVolumeParams (48 bytes)");

/// @note AdvancedGraphicsCB — IBL・SSR・TAA・GTAO・Contact Shadow 等の詳細設定。
/// @note LAYOUT: Constants.hlsli の AdvancedGraphicsConstants cbuffer と完全に一致させること。
/// @note 16-byte アライメント制約のため、各グループを 4 要素単位でまとめる。
struct AdvancedGraphicsCB {
    /// @note IBL
    float iblIntensity; ///< 環境光をシーンの照明に加える強さ。
    float iblDiffuseScale; ///< IBL による拡散光の倍率。
    float iblSpecularScale; ///< IBL による鏡面反射の倍率。
    int iblMaxMipLevel; ///< 鏡面 IBL の粗さに対応する最も粗いミップレベル。
    /// @note SSR
    float ssrMaxDistance; ///< スクリーン空間反射の光線を追跡する最大距離。
    float ssrThickness; ///< 深度画像で反射光線と面の交差を認める厚み。
    int ssrSteps; ///< スクリーン空間の反射光線を追跡するステップ数。
    float ssrIntensity; ///< スクリーン空間反射の合成強度。
    /// @note Volumetric
    float volLightIntensity; ///< 体積光の散乱結果に掛ける倍率。
    float volScattering; ///< 体積光の散乱を評価する係数。
    int volSteps; ///< 体積光を視線に沿って積分するステップ数。
    float volMaxDist; ///< 体積光を評価する視線上の最大距離。
    /// @note TAA
    float taaFeedback; ///< TAA 合成で過去の色を残す割合。
    float taaJitterX; ///< TAA の射影を横方向へずらす量。
    float taaJitterY; ///< TAA の射影を縦方向へずらす量。
    float taaPreviousOrthographic = 0.0f; ///< TAA の履歴が正射影で記録されたことを示す値。
    /// @note Motion Blur
    /// @note screenWidth/screenHeight は MotionBlur CS が b5 非バインド下で screenSize の代替として参照する
    float motionBlurStrength; ///< 画面上の速度をブラーの長さに反映する倍率。
    int motionBlurSamples; ///< 速度方向に参照するカラー画像のサンプル数。
    float screenWidth; ///< 参照する描画面の横方向の画素数。
    float screenHeight; ///< 参照する描画面の縦方向の画素数。
    /// @note GTAO
    float gtaoIntensity; ///< GTAO の遮蔽結果を合成する強さ。
    float gtaoRadius; ///< GTAO で面の周囲を調べる半径。
    int gtaoSlices; ///< GTAO の遮蔽を調べる断面の数。
    int gtaoStepsPerSlice; ///< 一つの断面で深度を調べるステップ数。
    /// @note Contact Shadows
    float contactShadowStrength; ///< 接触影による暗化の強さ。
    float contactShadowRayLen; ///< 接触影を調べる光線の長さ。
    int contactShadowSteps; ///< 接触影の深度を調べるステップ数。
    float contactShadowThick; ///< 画面深度との交差を接触影として扱う厚み。
    /// @note Lens Flare
    float lensFlareIntensity; ///< レンズフレアをシーンに加える強さ。
    int lensFlareGhostCount; ///< フレアに含めるゴースト像の数。
    float lensFlareHaloWidth; ///< フレアの光輪の幅。
    float lensFlareDistort; ///< レンズフレアの各色のずれ量。
    /// @note PCSS
    float pcssLightRadius; ///< PCSS の半影幅の計算に使う光源半径。
    int pcssEnabled; ///< 接触部と遠方で半影幅を変える PCSS を使用するか。
    float taaPreviousNearZ = 0.1f; ///< 履歴を記録したカメラの近クリップ距離。
    float taaPreviousFarZ = 1000.0f; ///< 履歴を記録したカメラの遠クリップ距離。
    /// @note LUT / 天候 (weather* は LUT ブロックの空き 3 枠を流用。Constants.hlsli 側も同じ)
    float lutBlend; ///< カラーグレーディング LUT の適用率。
    float weatherWetness; ///< 天候が材質の濡れに与える寄与。
    float weatherDarkening; ///< 濡れた面を暗くする強さ。
    float weatherPuddle; ///< 天候による水たまりの寄与。
    /// @note Reprojection 行列 (TAA / Motion Blur 共用)
    math::Matrix4 prevViewProjection; ///< 現フレームのワールド位置を履歴画像上の位置へ投影する前フレームの行列。
    math::Matrix4 invPrevViewProjection; ///< 前フレームのクリップ座標をワールド座標に戻す逆変換。
    /// @note Volumetric Lighting (拡張分)
    float volMinDist; ///< 体積光の積分を開始する距離。
    float volDensity; ///< 体積光を散乱させる媒体の密度。
    float volHeightFalloff; ///< 高度による体積媒体の減衰率。
    float volHeightStart; ///< 体積媒体の高度減衰の基準となる高さ。
    float volTintR; ///< 体積光の色補正に使う赤成分。
    float volTintG; ///< 体積光の色補正に使う緑成分。
    float volTintB; ///< 体積光の色補正に使う青成分。
    float volEdgeFade; ///< 体積光の輪郭をなじませる減衰量。
    /// @note 自動露出。autoExposureKey <= 0 で無効 (Composite が b5 の exposure をそのまま使う)。
    float autoExposureKey = 0.0f; ///< 自動露出が基準にする明るさ。正値のとき有効。
    float autoExposureCompensation = 0.0f; ///< 自動露出の目標値に加える補正（段）。
    float autoExposureMinEV = -8.0f; ///< 自動露出の許容範囲の下端。
    float autoExposureMaxEV =  8.0f; ///< 自動露出の許容範囲の上端。
    /// @note Forward のマテリアルが画面空間 AO / 接触影をどれだけ受けるか。0 で引かない。
    /// @note Deferred では DeferredLighting が適用するので 0 を入れる (二重適用の防止)。
    float screenAoStrength = 0.0f; ///< 記録済みの画面空間 AO を照明に反映する強さ。
    float screenContactShadowStrength = 0.0f; ///< 記録済みの画面空間の接触影を照明に反映する強さ。
    /// @note バッファ解像度 / 描画解像度。AO と接触影は半解像度で焼かれるので 0.5。
    float screenAoScale = 1.0f; ///< 描画解像度に対する画面空間 AO バッファの解像度比。
    float screenContactShadowScale = 1.0f; ///< 描画解像度に対する接触影バッファの解像度比。
    /// @note Light Probe Volume。[0] が内側 (t22)、[1] が外側 (t21)。
    /// @see Assets/Shaders/Rendering/LightProbeGI.hlsli
    ProbeVolumeParamsCB probeVolumes[2]{}; ///< ビューに採用した照明プローブ領域の配置と強度。
    float probeSpecularOcclusion = 0.0f;     ///< プローブの暗さを鏡面 IBL へ移す強さ [0,1]
    uint32_t probeVisibilityIndices[2] = { renderer::INVALID_BINDLESS_INDEX, renderer::INVALID_BINDLESS_INDEX }; ///< プローブの可視性を参照するテクスチャの識別番号。
    float _probePad = 0.0f; ///< HLSL 側の定数配置に合わせて確保する予約領域。
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
    float texelSize[2]; ///< このパスに設定した描画寸法の逆数。Bloom の縮小段では書き込み先の 1 画素に相当する。
    float screenSize[2]; ///< 対象とする画面の幅と高さ（画素）。
    float exposure; ///< トーンマッピング前の HDR 色に適用する露出倍率。
    float time; ///< 時間変化する画面効果に渡す経過時間。
    float fogDensity; ///< フォグの濃度を距離による減衰へ変換する係数。
    float bloomIntensity; ///< ブルーム画像をシーンに加算する強さ。
    float fogColor[3]; ///< 距離に応じて画面に合成する霧の RGB 色。
    float fogFar; ///< フォグの距離計算で使う遠方の境界。
    float contrast; ///< 中間調からの明暗差を調整する倍率。
    float saturation; ///< 色を輝度からどれだけ離すかを決める彩度の倍率。
    float hueShift; ///< 画面全体の色相に加える回転量。
    float temperature; ///< カラーグレーディングで暖色または寒色へ寄せる量。
    float tint; ///< カラーグレーディングの色かぶり補正量。
    float vignetteIntensity; ///< 画面外縁を暗くする強さ。
    float vignetteSmoothness; ///< ビネットの中心から周縁に向かう遷移の滑らかさ。
    float vignetteRoundness; ///< ビネットの輪郭の丸み。
    float vignetteColor[3]; ///< ビネットで画面外縁に重ねる RGB 色。
    float filmGrainIntensity; ///< 画面に重ねる粒状ノイズの強さ。
    float filmGrainResponse; ///< 元の輝度に応じて粒状ノイズの寄与を変える量。
    float chromaticAberration; ///< RGB チャンネルをずらす色収差の強さ。
    float lensDistortion; ///< レンズを模した画面座標の歪み量。
    float ssaoIntensity; ///< スクリーン空間の環境遮蔽を適用する強さ。
    float customIntensity; ///< カスタム画面効果に渡す強度。
    float customBlend; ///< 元画像に対してカスタム効果の出力を混ぜる割合。
    float _customPad[2]; ///< HLSL 側の定数配置に合わせて確保する予約領域。
    float customParameters[4]; ///< カスタムポストシェーダーに渡す最初の 4 成分。
    float underwaterStrength; ///< 水中表示を画面へ適用する強さ。
    float underwaterDepth; ///< 水中表示が参照するカメラの水深。
    float _underwaterPad[2]; ///< HLSL 側の定数配置に合わせて確保する予約領域。
    float underwaterColor[3]; ///< 水中フォグで散乱光に使う RGB 色。
    float underwaterFogDensity; ///< 水中フォグの密度。
    float sharpenStrength; ///< 隣接画素との明暗差を強調する量。
    float sharpenRadius; ///< 鮮鋭化で周囲を参照する範囲。
    float dofFocusDistance; ///< カメラから被写界深度の合焦面までの距離。
    float dofFocusRange; ///< 合焦面の前後で鮮明に保つ距離の範囲。
    float dofBlurRadius; ///< 焦点外の画素をぼかす半径。
    float sepiaIntensity; ///< セピア変換した色を元の色に混ぜる割合。
    float invertIntensity; ///< 反転色を元の色に混ぜる割合。
    float posterizeLevels; ///< ポスタライズで色を量子化する段階数。
    float pixelSize; ///< ピクセル化で一つの色にまとめる領域の大きさ。
    float _stylizedPad[3]; ///< HLSL 側の定数配置に合わせて確保する予約領域。
    float bloomThreshold; ///< ブルームに寄与する高輝度域の下限。
    float bloomSoftKnee; ///< しきい値付近の明るさをブルームへ滑らかにつなぐ幅。
    float clarityStrength; ///< 局所コントラストを強調する量。
    float clarityRadius; ///< 局所コントラストの評価範囲。
    float shadowLift; ///< カラー補正で暗部の明るさを持ち上げる量。
    float highlightCompression; ///< カラー補正で明部の階調を圧縮する量。
    float colorFilterIntensity; ///< RGB フィルター色を画面に反映する強さ。
    float _qualityPad0; ///< HLSL 側の定数配置に合わせて確保する予約領域。
    float colorFilter[3]; ///< カラーグレーディングに掛ける RGB フィルター色。
    /// @note Option の「明るさ」。colorFilter の 4 成分目の空きを使うのでレイアウトは変わらない。
    /// @note 1.0 で無加工。PostProcCB を zero-init する場所では 0 (= 真っ黒) になるので、
    /// @note Composite 以外のパスで読むなら必ず明示的に埋めること。
    float userBrightness;
    /// @note 画面フェード — Composite パスの最終出力に適用する。alpha=0 で通常, 1 で全面フェード色。
    float screenFadeColor[3]; ///< 画面フェードの RGB 色。
    float screenFadeAlpha; ///< 指定色で画面を覆う割合。
    /// @note 大気フォグ統合 (環境システム §3-3): フォグ色の出どころ。0=指数(従来), 1=大気散乱(エアリアル)。
    /// @note 末尾に追加し、既存フィールドのオフセット (HLSL PostProcConstants と一致) を変えない。
    float fogSource;
    /// @note 放射ブラー (VFXScreenEffect)。画面中心から外へ引き伸ばす量 [0,1]。
    /// @note 既存の padding を名前付きにするだけ。レイアウトを変えると Constants.hlsli の
    /// @note 4 コピーを同時に直す必要があり、1 つ漏らすとそのプロジェクトだけ値がずれる。
    float radialBlur;
    float _fogPad[2]; ///< HLSL 側の定数配置に合わせて確保する予約領域。
    /// @note 投影コースティクス改良 (Phase C-2): 水域 XZ 範囲フェード + 波連動 UV ゆらぎ。
    /// @note HalfExtent<=0 で範囲無制限 (後方互換)。WaveAmp=0 でゆらぎ無し。
    float causticsCenterX; ///< 水底に投影する集光模様の中心のワールド X 座標。
    float causticsCenterZ; ///< 水底に投影する集光模様の中心のワールド Z 座標。
    float causticsHalfExtentX; ///< 集光模様の適用範囲の X 方向の半幅。
    float causticsHalfExtentZ; ///< 集光模様の適用範囲の Z 方向の半幅。
    float causticsWaveAmp; ///< 水底の集光模様を揺らす波の振幅。
    float causticsWaveFreq; ///< 集光模様を揺らす波の周波数。
    float causticsWaveSpeed; ///< 集光模様を揺らす波の進行速度。
    float _causticsPad; ///< HLSL 側の定数配置に合わせて確保する予約領域。
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
    float shockRingCenter[2]; ///< 衝撃波リングを描く画面上の中心。
    float shockRingRadius; ///< 衝撃波リングの中心からの半径。
    float shockRingWidth; ///< 衝撃波の歪みを適用する帯の幅。
    float shockRingAmplitude; ///< 衝撃波の帯で画像をずらす振幅。
    /// @note 今フレーム生成したレイ反射を読むときだけ 1。既定 0 は未束縛 SRV を参照しない。
    float rayReflectionEnabled = 0.0f;
    /// @note ReflectionResolveStage: legacy0 / final1 / baseline source2. Primary glass replaces full surface radiance only in final.
    float reflectionResolveEnabled = 0.0f;
    /// @note このビューで SSR の記録に成功したときだけ読み、有効性と材質応答を分離する。
    float reflectionSsrEnabled = 0.0f;
    /// @note 水面と共有するRGB係数 [m^-1]。mode=0は旧水中表示を維持する。
    float underwaterAbsorption[3]{}; ///< 水中を進む光の RGB ごとの吸収係数（1/m）。
    float underwaterOpticalMode = 0.0f; ///< 水中の吸収と散乱に使う描画方式。
    float underwaterScattering[3]{}; ///< 水中を進む光の RGB ごとの散乱係数（1/m）。
    float underwaterPhaseG = 0.0f; ///< 水中粒子の散乱の方向性を決める位相関数の g。
    /// @note カメラ列の水平slabと積分距離 [m]。Splineの遠方側面はslab近似。
    float underwaterSurfaceY = 0.0f;
    float underwaterBottomY = 0.0f; ///< 水中の光を積分する範囲の下端のワールド Y 座標。
    float underwaterMaxDistance = 0.0f; ///< 水中媒体を通る光の積分距離の上限。
    float underwaterLightStrength = 0.0f; ///< 水中の散乱光の寄与に掛ける倍率。
    float underwaterWaveAmplitude = 0.0f; ///< 水中表示の揺らぎに使う波の振幅。
    float underwaterWaveFrequency = 0.0f; ///< 水中表示の揺らぎに使う波の周波数。
    float underwaterWaveSpeed = 0.0f; ///< 水中表示に使う波の進行速度。
    float underwaterDistortion = 0.0f; ///< 水中で背景画像を歪ませる強さ。
    float underwaterCenter[2]{}; ///< 水域の水平面上の中心座標。
    float underwaterHalfExtent[2]{}; ///< 水域の水平面上の半幅と半奥行き。
    float underwaterCosYaw = 1.0f; ///< 水域の水平面上の回転角の余弦。
    float underwaterSinYaw = 0.0f; ///< 水域の水平面上の回転角の正弦。
    float underwaterLightSteps = 48.0f; ///< 水中散乱光を積分するサンプル数。
    /// @note 当該ビューで今フレームの水中散乱を記録した場合だけSRVを読む。
    float underwaterLightReady = 0.0f;
    /// @note Two 16-bit rows per word. Set bits certify tiles outside all optical water.
    std::array<uint32_t, 8> waterOpticalDryTiles{};
};
/// @note HLSL 側 (Common/Constants.hlsli の PostProcConstants) は複数コピーある。サイズがずれたら全コピーを直す。
static_assert(sizeof(PostProcCB) == 544, "PostProcCB must match PostProcConstants in Constants.hlsli (544 bytes)");

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
    math::Vector4 color; ///< 描画する輪郭線の色。
    float         width; ///< 輪郭の幅を決める値。
    float         _pad[3]; ///< HLSL 側の定数配置に合わせて確保する予約領域。
};
static_assert(sizeof(OutlineCB) == 32, "OutlineCB must match OutlineConstants in SelectionOutline.hlsl (32 bytes)");

/// @note ObjectMaskConstants (b2) — Pipeline/Mask/ObjectMask*.hlsl と一致させること。
/// @note payload … そのままマスクへ書く RGBA (意味は書き手と読み手の取り決め)
/// @note flags.x … 1 = 手前に何かある画素を捨てる (visibleOnly)
struct ObjectMaskCB {
    math::Vector4 payload; ///< 輪郭マスクに書き込む色と強度等の 4 成分。
    math::Vector4 flags; ///< x が正のとき、可視面だけを輪郭マスクに含める。
};
static_assert(sizeof(ObjectMaskCB) == 32, "ObjectMaskCB size mismatch");

/// @note デカールの受信レイヤーフィルタが有効であることを示す DecalCB::flags のビット。
inline constexpr uint32_t kDecalFlagReceiverFilter = 1u;

/// @note DecalConstants (b10) — Material/Decal/DecalCommon.hlsli と完全に一致させること。
/// @note b2 は "MaterialConstants" として .mat のリフレクション対象なので、投影データを
/// @note そこへ置くとデカールだけマテリアルを持てない例外になる (Common/Binding.hlsli 参照)。
struct DecalCB {
    math::Matrix4 invDecalWorld; ///< 面のワールド座標をデカールの投影範囲へ移す逆行列。
    math::Vector3 decalTangent; ///< デカールの接線方向。法線マップの向きをワールド空間に合わせる。
    float         _pad0 = 0.0f; ///< HLSL 側の定数配置に合わせて確保する予約領域。
    math::Vector3 decalBitangent; ///< デカールの従接線方向。
    float         _pad1 = 0.0f; ///< HLSL 側の定数配置に合わせて確保する予約領域。
    math::Vector3 decalNormal; ///< デカールの投影面の法線方向。
    float         _pad2 = 0.0f; ///< HLSL 側の定数配置に合わせて確保する予約領域。
    float         alpha = 1.0f; ///< デカールを受け面に重ねる不透明度。
    /// @note 角度フェード。受け面の法線が投影軸から傾くほどデカールを薄くする。
    /// @note OBB 投影は斜めの面でテクスチャが引き伸ばされて長い筋になる (壁の角をまたいだ
    /// @note 着弾痕が「伸びた汚れ」に見える)。角度で薄めれば破綻する範囲がそのまま消える。
    float         angleFadeStrength = 1.0f; ///< 0 = フェードなし
    float         angleFadeCos      = 0.34f; ///< この cos より寝た面では完全に消える (既定 70 度)
    uint32_t      flags             = 0; ///< レイヤーフィルターなどデカール描画の設定ビット。
    uint32_t      receiverLayerMask = ~0u; ///< デカールの投影を許可する受け面のレイヤービット。
    /// @note フリップブック。1 コマぶんの UV スケールと、今のコマ番号。
    /// @note 無効時は (1, 1) / 0 で、シェーダー側の変換が恒等になる。
    /// @note «スケール + 番号» にするのは旧 _pad3 の 3 float に収まり cbuffer サイズが
    /// @note 変わらないため。伸ばすと DecalCommon.hlsli の 4 コピーと static_assert を
    /// @note 同時に直す必要があり、直し忘れると黙って別の値を読む。
    float         frameScale[2] = { 1.0f, 1.0f };
    float         frameIndex    = 0.0f; ///< シートアニメーションで表示するコマ番号。
};
static_assert(sizeof(DecalCB) == 144, "DecalCB size mismatch");

/// @note 組み込み Decal.hlsl の MaterialConstants (b2)。
/// @note .mat を割り当てていない DecalComponent へは DecalPass がこの形で値を流す。
/// @note LAYOUT: Assets/Shaders/Material/Decal/Decal.hlsl と完全に一致させること。
struct DecalMaterialCB {
    float    albedoTint[4]    = { 1.0f, 1.0f, 1.0f, 1.0f }; ///< ベース色テクスチャに掛ける色とアルファ。
    float    emissiveColor[3] = { 1.0f, 1.0f, 1.0f }; ///< 発光に使う RGB 色。
    float    emissiveScale    = 0.0f; ///< 発光色の強度に掛ける倍率。
    float    uvTiling[2]      = { 1.0f, 1.0f }; ///< テクスチャを繰り返す U、V 方向の倍率。
    float    uvOffset[2]      = { 0.0f, 0.0f }; ///< テクスチャ座標に加える U、V 方向のずれ。
    float    normalStrength   = 1.0f; ///< 受け面の照明にデカール法線を反映する強さ。
    /// @note Material::Upload と同じ規則: bit i = テクスチャスロット i が有効。
    /// @note bit0=albedo(t0) bit1=normal(t1) bit3=emissive(t3)
    uint32_t textureMask      = 0;
    float    _pad[2]          = { 0.0f, 0.0f }; ///< HLSL 側の定数配置に合わせて確保する予約領域。
};
static_assert(sizeof(DecalMaterialCB) == 64, "DecalMaterialCB size mismatch");

/// @note 受信レイヤーバッファを描くドローの b10。DecalMask(.Skinned).hlsl と一致させること。
struct DecalReceiverCB {
    /// @note レイヤー番号 + 1。0 はクリア値 (未描画) と衝突するため使わない。
    float layerEncoded = 0.0f;
    float _pad[3]      = { 0.0f, 0.0f, 0.0f }; ///< HLSL 側の定数配置に合わせて確保する予約領域。
};
static_assert(sizeof(DecalReceiverCB) == 16, "DecalReceiverCB size mismatch");

/// @note 公開ヘッダーに置く理由: Particle / Trail / Terrain / Water は «エンジンが埋める»
/// @note 定数バッファを持ち、材質の b2 とは別枠。src/ に閉じると、マテリアルプレビュー等
/// @note «本編と同じ絵を焼きたい» 側が写しを持つしかなく、正本が 2 つに分かれる。

/// @note パーティクルビルボード頂点。Particle.hlsl の ParticleVSIn と一致させること。
struct ParticleVertex {
    float center[3];  ///< POSITION   12 bytes
    float uv[2];      ///< TEXCOORD0   8 bytes
    float color[4];   ///< COLOR       16 bytes
    float size;       ///< TEXCOORD1    4 bytes
    float rotation;   ///< TEXCOORD2    4 bytes
    float uvRect[4];  ///< TEXCOORD3   16 bytes
    float velocity[3];///< TEXCOORD4   12 bytes
    float nextUvRect[4]; ///< TEXCOORD5 16 bytes
    float spriteBlend;   ///< TEXCOORD6  4 bytes
};                       ///< 92 bytes
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
inline constexpr std::size_t kParticleConstantSlot = cb::PARTICLE;
/// @note six-way 色アトラスは Particle pass 専用で、Bloom / Environment map と時分割する。
inline constexpr std::size_t kParticleSixWayAlbedoColorSlot = srv::PARTICLE_SIX_WAY_COLOR;
inline constexpr std::size_t kParticleSixWayEmissionColorSlot = srv::PARTICLE_SIX_WAY_EMISSION;

/// @note Particle描画専用CB (b11)。CPU/GPUシェーダーで同じRenderer設定を使う。
struct ParticleRenderCB {
    uint32_t renderMode = 0; ///< 粒子をビルボード等のどの方式で描くかを示す値。
    float stretchedVelocityScale = 0.1f; ///< 粒子の速度を伸びの長さに反映する倍率。
    float stretchedLengthScale = 1.0f; ///< 引き伸ばしビルボードの長手方向の倍率。
    float softParticleFadeDistance = 0.5f; ///< シーン深度に近づいた粒子を滑らかに消す距離。
    uint32_t softParticles = 0; ///< シーンとの交差部で粒子を薄くする機能を使うか。
    uint32_t maxParticles = 0; ///< 描画時に参照できる GPU 粒子数の上限。
    uint32_t effectsFlags = 0; ///< bit0 distortion / bit1 six-way lighting / bit2 motion-vector flipbook
    float distortionStrength = 0.015f; ///< 粒子による背景画像の屈折の強さ。
    float lightingStrength = 1.0f; ///< 粒子色に照明を反映する強さ。
    float emissiveScale = 1.0f; ///< 粒子の発光色に掛ける倍率。
    float motionVectorStrength = 1.0f; ///< フリップブック補間で速度テクスチャを適用する倍率。
    /// @note 描画先の解像度 [px]。
    /// @note 歪みはシーンカラーを画面 UV でサンプルするが、screenSize は PostProcConstants
    /// @note 側にありパーティクル描画はそれをバインドしない。未設定だと UV が saturate で
    /// @note 右下隅へ張り付き、屈折でなくべた塗りになるため、解像度はここから渡す。
    float screenWidth = 1.0f; ///< 参照する描画面の横方向の画素数。
    float screenHeight = 1.0f; ///< 参照する描画面の縦方向の画素数。
    /// @note ビルボードの軸ごとのサイズ倍率 (ParticleEmitter::sizeAxisScale の xy)。
    /// @note 縦横比はエミッター単位の値なので粒子ごとに持たせず CB で渡す。CPU 頂点
    /// @note フォーマットも GPU の GpuParticle も太らせずに、CPU/GPU 双方の描画へ
    /// @note 同じ 1 か所から効かせられる。
    float sizeAxisScaleX = 1.0f; ///< 粒子のビルボードを横方向に伸縮する倍率。
    float sizeAxisScaleY = 1.0f; ///< 粒子のビルボードを縦方向に伸縮する倍率。
    /// @note 受け影の強さ [0,1]。有効/無効は effectsFlags の bit3 で判定する。
    float shadowStrength = 1.0f; ///< 粒子が受ける影による暗化の強さ。
    /// @note ボリュメトリック煙 (effectsFlags bit4)。ビルボード内で球状密度場をレイマーチする。
    uint32_t volumetricSteps = 8;      ///< 視線方向のサンプル数
    float volumetricDensity = 1.0f;    ///< 消衰係数。大きいほど濃く不透明になる
    float volumetricAnisotropy = 0.3f; ///< Henyey-Greenstein g。正で前方散乱 (逆光で縁が光る)
    float volumetricNoiseScale = 2.0f; ///< 密度ノイズの空間周波数 [1/m]
    /// @note GPU ソート済みインデックス (t15) が有効か。GPU 経路の VS だけが読む。
    /// @note シェーダー分岐ではなく定数で切り替えるのは、ソート無効時に t15 へ何も
    /// @note バインドしない構成を許すため (未バインド SRV の読みを踏まない)。
    uint32_t gpuSortEnabled = 0; ///< 描画順の参照に GPU の並べ替え結果を使うか。
    /// @note 自己影の消衰係数。0 で無効。光源側密度バッファ (t9) を引いて透過率へ変換する。
    /// @note 受け影は「他の物体が落とす影」しか扱えない。粒子群が自分へ落とす影が無いと、
    /// @note 厚みのある煙・雲は光の当たり方が一様になり平坦な塊に見える。
    float selfShadowStrength = 0.0f; ///< 粒子がほかの粒子に落とす影の強さ。
    /// @name 煙の散乱 (effectsFlags bit1 有効時)
    /// @{
    /// @note 素の N・L は不透明な球の陰影で、光を透かす媒質には合わない。巻き込み拡散で
    /// @note 陰側の黒潰れを避け、前方散乱で逆光時に縁が光るようにする。
    float smokeWrap = 0.5f; ///< 光が煙の陰側へ回り込む度合い。
    float smokeTransmission = 0.0f; ///< 逆光が煙を通過する寄与の強さ。
    /// @note HLSL の cbuffer では float4 が 16 バイト境界を跨げない。ここまでで offset 96 に
    /// @note 揃えてあるので、この 2 つを動かすとシェーダー側と黙ってずれる。
    math::Vector4 tintColor = { 1.0f, 1.0f, 1.0f, 1.0f }; ///< .mat の albedo (リニア済み)
    float smokeBackScatterPower = 4.0f; ///< 煙の逆光散乱の広がりを決める指数。
    float distortionChromatic = 0.0f; ///< 屈折で RGB ごとのずれを付ける量。
    /// @note カメラ距離フェード [m]。near 未満で 0、far 以上で 1 の不透明度になる。
    /// @note 0 / 0 (既定) で無効。near == far も無効扱い (0 除算になる)。
    /// @note 一人称の近距離で粒子が «顔に張り付いて画面を覆う» のを、粒子側のサイズや
    /// @note 寿命を変えずに消せる唯一の手段。pad 枠の転用なので CB サイズは動かない。
    float cameraFadeNear = 0.0f; ///< カメラ付近で粒子をフェードさせる範囲の近端。
    float cameraFadeFar = 0.0f; ///< カメラ付近で粒子をフェードさせる範囲の遠端。
    math::Vector4 sixWayEmission = { 0.0f, 0.0f, 0.0f, 0.0f }; ///< rgb = 6-way マップの発光色 (リニア HDR)
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
    math::Vector4 gradientColors[8]{}; ///< リニア化済み
    /// @note 8 個のキー時刻。float4 × 2 に詰めるのは、HLSL の cbuffer が float の配列を
    /// @note 1 要素 16 バイトへ膨らませるため (float times[8] は 128 バイトを食う)。
    math::Vector4 gradientTimes[8 / 4]{};
    std::uint32_t gradientKeyCount = 0;
    /// @note ParticleCurveInterpolation の値 (0=Linear / 1=Step / 2=Smooth)。
    std::uint32_t gradientInterpolation = 0;
    std::uint32_t _gradientPad[2]{}; ///< HLSL 側の定数配置に合わせて確保する予約領域。
};
/// @note Trail.hlsl の gTrailFlags と一致させること。
inline constexpr std::uint32_t kTrailFlagSrgbTexture = 1u;

/// @note テクスチャが sRGB でエンコードされているかを .meta から引く。
/// @note このエンジンは _SRGB フォーマットの SRV を作らず「シェーダーが自分で
/// @note SRGBToLinear する」規約で統一されているため、素材ごとに判定が要る
/// @note (手描きは sRGB、ProceduralVFXTextures はリニアで焼くため一律に決められない)。
[[nodiscard]] FBZZ_GRAPHICS_API bool IsEffectTextureSrgb(const std::string& texturePath);
static_assert(sizeof(TrailCB) == 224, "TrailCB layout mismatch");

/// @brief Terrain シェーダーの b1。
/// @note LAYOUT: Assets/Shaders/Terrain/TerrainSurface.hlsli の TerrainCB と完全に一致させること。
/// @note 層ごとの値は StructuredBuffer (TerrainLayerGpu) へ移した。CB の固定長配列では層数に上限が残る。
/// @see Docs/design/terrain-layers.md
struct TerrainObjectCB {
    math::Matrix4 worldMatrix; ///< モデルのローカル座標からワールド座標への変換行列。
    math::Matrix4 wvpMatrix; ///< ローカル座標を描画先のクリップ座標に変換する合成行列。
    /// @brief 天候 (x=wetness, y=darkening, z=puddleAmount) と材質 LOD の到達距離 (w, world m, 0 で無効)。
    /// @note 地形シェーダーは b1 を TerrainCB に使うため b8 を宣言できず、値はここで手渡す。
    math::Vector4 weather;
    /// @brief x=ローカル幅 X [m], y=ローカル奥行 Z [m], z=heightBlendDepth, w=自動ブレンドを持つ層があるか (0/1)。
    math::Vector4 terrainParams;
    std::uint32_t layerBufferIndex = 0xFFFFFFFFu; ///< TerrainLayerGpu 配列の bindless 添字
    std::uint32_t layerCount       = 0;
    std::uint32_t splatColumns     = 1;
    std::uint32_t splatRows        = 1;
    /// @note 非一様スケールと親からの shear に対応する法線の変換。
    math::Matrix4 worldInvTranspose = math::Matrix4::Identity(); ///< 非一様スケールを含むワールド変換に合わせて法線を変換する逆転置行列。
    math::Vector4 grassGroundColor{}; ///< RGB は線形、W は地表に混ぜる強度。
    math::Vector4 grassGroundLod{}; ///< 開始距離と最大寄与距離 [world m]、塗り層番号、層しきい値。
    math::Vector4 grassGroundWaterClip{}; ///< x は最低の world Y [m]、y は Horizon によるクリップの有効フラグ。
};
static_assert(sizeof(TerrainObjectCB) == 288, "TerrainObjectCB size mismatch");

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
    /// @note x=スプラインの水深 [m]、yz=ワールド XZ 流速 [m/s]、w=スプライン頂点なら 1。
    math::Vector4 splineData{};
    /// @note Two packed triangle profile index triples, corner plus one, and source weight.
    math::Vector4 zoneMaterial{};
    math::Vector2 riverTravelTimes{}; ///< Signed fundamental and short-band travel times [s].
};
static_assert(sizeof(WaterVertex) == 60, "WaterVertex size mismatch");

/// @note WaterCB — Water シェーダーの b1。
/// @note LAYOUT: Assets/Shaders/Water/Water.hlsl の WaterCB と完全に一致させること。
struct WaterMaterialBlendTarget {
    math::Vector4 shallowColorDepth{};
    math::Vector4 deepColorDepth{};
    math::Vector4 surface{};
    math::Vector4 normalReflectionRefraction{};
    math::Vector4 detail{};
    math::Vector4 foam{};
    math::Vector4 crestFoam{};
    math::Vector4 foamColorRoughness{};
    math::Vector4 absorptionPhase{};
    math::Vector4 scatteringMode{};
    math::Vector4 sss{};
    math::Vector4 effects{};
    math::Vector4 skyTint{};
    math::Vector4 rippleColor{};
};
static_assert(sizeof(WaterMaterialBlendTarget) == 224);

inline constexpr uint32_t WATER_RIVER_WAVE_ROCK_COUNT = 8;

struct WaterCB {
    math::Matrix4 worldMatrix; ///< モデルのローカル座標からワールド座標への変換行列。
    math::Matrix4 wvpMatrix; ///< ローカル座標を描画先のクリップ座標に変換する合成行列。
    math::Vector4 shallowColorDepth;
    math::Vector4 deepColorDepth;
    math::Vector4 surfaceParams;
    math::Vector4 normalParams;
    math::Vector4 timeParams;
    math::Vector4 foamParams;
    math::Vector4 refractionFlowParams;
    math::Vector4 waveDir[16];
    math::Vector4 waveParams[16];
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
    /// @note xy/zw は水域全体に対するパッチの UV 最小/最大。patchCellSize.z=0 は旧格子。
    math::Vector4 patchRect{};
    math::Vector4 patchCellSize{};
    /// @note MIN_X/MAX_X/MIN_Z/MAX_Z の世界セル幅。角は incident patch の最大幅を使う。
    math::Vector4 patchEdgeCellSize[4]{};
    math::Vector4 patchCornerCellSize[4]{};
    /// @note x/y=正の波高比による白波の開始/遷移幅、z=強度、w=泡の減衰時間 [s]。
    math::Vector4 crestFoamParams{};
    /// @note xyz=泡の線形アルベド、w=泡の粗さ。
    math::Vector4 foamColorRoughness{};
    /// @note x=Horizon 有効、y=Horizon パッチ、z=局所波紋の減衰距離 [m]、w=媒体 owner ID。
    math::Vector4 farOceanParams{};
    /// @note x=地形情報有効、y=符号付き水深の復号範囲 [m]、zw=情報テクスチャの寸法。
    math::Vector4 terrainInfoParams{};
    /// @note 16基波の初期位相 [rad]。各Vector4に4基波を格納する。
    math::Vector4 wavePhasePacked[4]{};
    /// @note x=有効基波数、yz=さざ波の方向上書き (0 で基流)、w=明示count。
    math::Vector4 waveControls{};
    math::Vector4 absorptionCoefficients{};
    math::Vector4 scatteringCoefficients{};
    /// @note x=係数光学有効、y=Henyey–Greenstein非対称係数。
    math::Vector4 opticalParams{};
    math::Vector4 patchMorphView{};
    math::Vector4 patchMorphForward{};
    math::Vector4 patchMorphMetric{};
    math::Vector4 patchMorphParams{};
    math::Vector4 patchMorphCellSize{};
    /// @note x=浅水波の減衰開始深度 [m]、y=水深場有効、z=範囲外フェード幅 [m]、w=矩形水域の基流の符号付き速度 [m/s]。
    math::Vector4 waveDepthParams{};
    /// @note xy=パッチ全域の local XZ 最小値、zw=local 寸法。0 寸法は従来の有限矩形。
    math::Vector4 horizonPatchDomain{};
    /// @note xy=Terrain 域の中心、zw=寸法。Water 軸へ投影した world m。0 寸法は著者設定の矩形。
    math::Vector4 terrainDomain{};
    /// @note River cap xy=world XZ, z=blend radius [m], w=receiving sea level [m]. Zero radius disables a cap.
    math::Vector4 riverHorizonCaps[2]{};
    /// @note xy=world XZ, z=flat radius [m], w=outer fade radius [m]. A negative first w pins the whole water surface.
    std::array<math::Vector4, 4> waterfallSeams{};
    /// @note xy = endpoint XZ, zw = common flat/fade radii [m].
    math::Vector4 materialBlendMouth{};
    std::array<math::Vector4, 4> horizonWaveSeams{};
    math::Vector4 horizonLinkedRiverMedia{};
    WaterMaterialBlendTarget materialBlendTarget{};
    /// @note x=fundamental period [s], y=packed rock count, zw=representative fundamental and short wavelengths [m].
    math::Vector4 riverWaveParams{};
    /// @note Row pairs contain center XYZ with radius [m], followed by half height [m] in x.
    std::array<math::Vector4, 2u * WATER_RIVER_WAVE_ROCK_COUNT> riverWaveRocks{};
};
static_assert(sizeof(WaterCB) == 2656, "WaterCB size mismatch");

/// @note WaterEffectParams — Water.hlsl の MaterialConstants (b2) の C++ ミラー。
struct WaterEffectParams {
    float rimGlowStrength    = 0.40f; ///< 水面の縁を明るく見せる効果の強さ。
    float minShallowAlpha    = 0.65f; ///< 浅瀬を合成するときの不透明度の下限。
    float specularStrength   = 0.75f; ///< 水面に出る鏡面ハイライトの強さ。
    /// @note 旧 specularExponent。ハイライトの鋭さは smoothness から導出するため廃止した枠。
    float _pad0              = 0.0f;
    float skyReflectTint[3]  = { 0.45f, 0.82f, 1.0f }; ///< 空の反射光に掛ける RGB 色。
    /// @note 旧 envMapBlend。空反射の混合率は skyReflection が持つため廃止した枠。
    float _pad1              = 0.0f;
    float rippleRingColor[3] = { 0.88f, 0.97f, 1.0f }; ///< 波紋の輪に使う RGB 色。
    float rippleRingStrength = 0.72f; ///< 波紋の輪を水面に重ねる強さ。
};
static_assert(sizeof(WaterEffectParams) == 48, "WaterEffectParams layout mismatch with MaterialConstants");

struct RenderPassHandles {
    /// @note selectionMaskRT / outlineRT はここから外した。名前 "SelectionMask" / "Outline" を
    /// @note 申告したパスが res.Target() で引く。
    /// @note ここが «申告とは別» の第 2 の正本になると両者の食い違いに誰も気付けないため、
    /// @note 名前 1 本に寄せ切るまで引ける経路を順に閉じている。
    /// @note ランタイムの輪郭マスク (RGB=要求された色 / A=太さ)。エディタ選択のマスク
    /// @note («選ばれているか» の 1 ビット) とは別に持つ。
    renderer::ResourceHandle<renderer::RenderTargetTag> customPostProcessRT[2]; ///< ユーザー定義のポスト処理に使う描画ターゲットのハンドル。

    /// @note Bloom のミップ連鎖と、各段の実寸。テクセルサイズを CB へ入れるのに要る。
    /// @note GetDimensions で引けなくはないが、書き込み先しか分からないため
    /// @note 「読み込み元のテクセルサイズ」は CPU 側から渡す必要がある。
    renderer::ResourceHandle<renderer::TextureTag> bloomChain[kBloomMipCount];
    /// @note 足し戻しの書き先。読みながら書けないので、ダウンサンプル用とは別に持つ。
    renderer::ResourceHandle<renderer::TextureTag> bloomUpChain[kBloomMipCount];
    uint32_t bloomChainWidth[kBloomMipCount]  = {};
    uint32_t bloomChainHeight[kBloomMipCount] = {};
    renderer::ResourceHandle<renderer::TextureTag> bloomHalf;   ///< = bloomChain[0]
    renderer::ResourceHandle<renderer::TextureTag> ssaoRaw;
    renderer::ResourceHandle<renderer::TextureTag> shadowDepthTex;
    renderer::ResourceHandle<renderer::TextureTag> fxaaInput;
    renderer::ResourceHandle<renderer::TextureTag> postProcessInput;

    renderer::ResourceHandle<renderer::ShaderTag> bloomDownShader; ///< 高輝度成分の縮小と Bloom のミップ生成に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag> bloomUpShader; ///< 小さいミップからの Bloom の合成に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag> ssaoShader; ///< 画面深度からの環境遮蔽の算出に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag> ssaoBlurShader; ///< 画面空間の環境遮蔽の平滑化に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag> compositeShader; ///< 描画済みの各画像の最終合成に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag> causticsShader; ///< 水面を通過する集光模様の描画に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag> selectionMaskShader; ///< Editor で選択中の物体のマスク生成に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag> selectionMaskSkinnedShader; ///< 選択したスキニング形状のマスク生成に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag> selectionMaskParticleShader; ///< 選択した粒子のマスク生成に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag> selectionMaskParticleGpuShader; ///< 選択した GPU 粒子のマスク生成に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag> selectionOutlineShader; ///< 選択マスクを使った輪郭線の合成に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag> objectMaskShader; ///< ランタイム輪郭用のオブジェクトマスクの生成に使うシェーダーのハンドル。
    /// @note objectMaskShader のインスタンシング変種。
    /// @note 無効なら束ねずに 1 件ずつ出す (絵は変わらない)。
    /// @see Docs/design/gpu-instancing.md
    renderer::ResourceHandle<renderer::ShaderTag> objectMaskInstancedShader; ///< インスタンス描画時の輪郭マスクの生成に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag> objectMaskSkinnedShader; ///< スキニングした形状の輪郭マスクの生成に使うシェーダーのハンドル。
    /// @note 全画面コピー。読みながら書けない場所で «今の絵» を退避するのに使う。
    renderer::ResourceHandle<renderer::ShaderTag> copyColorShader; ///< 描画済みのカラー画像の転送に使うシェーダーのハンドル。
    /// @note 縮小して走ったカスタムパスの結果を実寸へ戻す。
    renderer::ResourceHandle<renderer::ShaderTag> customComposeShader; ///< カスタム効果の画像合成に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag> fxaaShader; ///< 画面空間の輪郭に対する FXAA 処理に使うシェーダーのハンドル。
    /// @note 内部解像度の最終画を出力先の実寸へ引き伸ばす。等倍のフレームでは使わない。
    renderer::ResourceHandle<renderer::ShaderTag> upscaleShader; ///< 描画した画像の出力解像度への拡大に使うシェーダーのハンドル。
    /// @note 内部解像度が出力より大きいとき (スーパーサンプリング) に、出力 1 画素の足跡を平均して縮める。
    renderer::ResourceHandle<renderer::ShaderTag> downscaleShader; ///< 描画結果の縮小に使うシェーダーのハンドル。
    std::vector<renderer::ResourceHandle<renderer::ShaderTag>> customPostProcessShaders;

    renderer::ResourceHandle<renderer::PipelineStateTag> selectionMaskPSO; ///< Editor で選択中の物体のマスク生成に使うパイプライン状態のハンドル。
    renderer::ResourceHandle<renderer::PipelineStateTag> postprocPSO; ///< ポストプロセスによる画面効果の合成に使うパイプライン状態のハンドル。
    renderer::ResourceHandle<renderer::PipelineStateTag> causticsPSO; ///< 水面を通過する集光模様の描画に使うパイプライン状態のハンドル。

    renderer::ResourceHandle<renderer::ConstantBufferTag> frameCB; ///< ビューとカメラのフレーム定数の転送に使う定数バッファのハンドル。
    renderer::ResourceHandle<renderer::ConstantBufferTag> objectCB; ///< オブジェクトごとの変換と描画設定の転送に使う定数バッファのハンドル。
    renderer::ResourceHandle<renderer::ConstantBufferTag> lightCB; ///< 光源情報の転送に使う定数バッファのハンドル。
    /// @note 最終フォールバックの単位行列パレット。スケルトンが解決できない場合のみ使う。
    /// @note 通常は Model::referencePoseCB (リファレンスポーズ) が優先される。
    /// @note ResolveSkinningCB() を必ず経由すること。
    renderer::ResourceHandle<renderer::ConstantBufferTag> bindPoseSkinningCB; ///< バインドポーズ基準のスキニングに使う定数バッファのハンドル。
    renderer::ResourceHandle<renderer::ConstantBufferTag> postprocCB; ///< ポストプロセスによる画面効果の合成に使う定数バッファのハンドル。
    renderer::ResourceHandle<renderer::ConstantBufferTag> outlineCB; ///< マスクからの輪郭描画に使う定数バッファのハンドル。
    renderer::ResourceHandle<renderer::ConstantBufferTag> objectMaskCB; ///< ランタイム輪郭用のオブジェクトマスクの生成に使う定数バッファのハンドル。

    /// @note 可視サーフェスのレイヤー番号 + 1 を持つ受信バッファ。レイヤーフィルタを持つ
    /// @note デカールが 1 つでもある フレームだけ描く。
    renderer::ResourceHandle<renderer::RenderTargetTag>   decalMaskRT; ///< デカールを受ける面のマスク生成に使う描画ターゲットのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag>         decalShader; ///< デカールの投影に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag>         decalMaskShader; ///< デカールを受ける面のマスク生成に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag>         decalMaskSkinnedShader; ///< スキニングした面のデカール受け取りマスク生成に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::PipelineStateTag>  decalPSO; ///< デカールの投影に使うパイプライン状態のハンドル。
    renderer::ResourceHandle<renderer::PipelineStateTag>  decalMaskPSO; ///< デカールを受ける面のマスク生成に使うパイプライン状態のハンドル。
    renderer::ResourceHandle<renderer::ConstantBufferTag> decalCB; ///< デカールの投影に使う定数バッファのハンドル。
    /// @note 組み込みシェーダー用の b2。.mat を持つデカールは Material 側の cbuffer を使う。
    renderer::ResourceHandle<renderer::ConstantBufferTag> decalMaterialCB; ///< デカール材質の色・法線の適用に使う定数バッファのハンドル。
    renderer::ResourceHandle<renderer::ConstantBufferTag> decalReceiverCB; ///< デカールを受ける面の条件の指定に使う定数バッファのハンドル。

    renderer::ResourceHandle<renderer::ShaderTag>         shadowShader; ///< 光源から見た深度の記録に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag>         shadowSkinnedShader; ///< スキニングした形状のシャドウ深度描画に使うシェーダーのハンドル。
    /// @note shadowShader のインスタンシング変種。world を b1 でなく VS の t0 から引く。
    /// @note 無効なら EmitShadowCasters は束ねずに 1 件ずつ出す (絵は変わらない)。
    /// @see Docs/design/gpu-instancing.md
    renderer::ResourceHandle<renderer::ShaderTag>         shadowInstancedShader; ///< インスタンスをまとめたシャドウ深度描画に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ConstantBufferTag> shadowCB; ///< 光源から見た深度の記録に使う定数バッファのハンドル。

    /// @name モーションベクター
    /// @{
    /// @note RG = 現 UV - 前フレーム UV、B = 書き込み済みフラグ。TAA / MotionBlur が t26 で読む。
    /// @note RGBA16F を使うのは CreateRenderTarget にフォーマット引数が無く RG16F を作れないため。
    renderer::ResourceHandle<renderer::ShaderTag>         velocityShader; ///< 画面上の移動量の記録に使うシェーダーのハンドル。
    /// @note velocityShader のインスタンシング変種。b1 の 2 枠目 (prevWorld) も per-instance で運ぶ。
    /// @note 無効なら束ねずに 1 件ずつ出す (絵は変わらない)。
    /// @see Docs/design/gpu-instancing.md
    renderer::ResourceHandle<renderer::ShaderTag>         velocityInstancedShader; ///< インスタンスごとの画面上の移動量の記録に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag>         velocitySkinnedShader; ///< スキニングした形状の画面上の移動量の記録に使うシェーダーのハンドル。
    /// @}

    /// @name Spot / Point シャドウ
    /// @{
    /// @note Directional の CSM (shadowMapRT) とは別のアトラス。t28 へ束縛する。
    /// @note CSM のタイル数は視錐台の分割で、こちらは影付きライトの本数で決まる。
    /// @note 面積を奪い合わせると、ライトを 1 つ置いただけで遠景カスケードが粗くなる。
    renderer::ResourceHandle<renderer::ConstantBufferTag> punctualShadowCB; ///< 局所光源のシャドウアトラスと投影の設定に使う定数バッファのハンドル。
    /// @}

    /// @name ライト Cookie (投影テクスチャ)
    /// @{
    /// @note 複数の Cookie を 1 枚へ敷き詰めたアトラス。t31 へ束縛する。
    /// @note 1 回の PS 呼び出しの中でまとめて評価するので、ライトごとに差し替えられない。
    renderer::ResourceHandle<renderer::ShaderTag>         cookieBlitShader; ///< ライトクッキーのアトラスへの転写に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ConstantBufferTag> cookieBlitCB; ///< ライトクッキーのアトラスへの転写に使う定数バッファのハンドル。
    renderer::ResourceHandle<renderer::PipelineStateTag>  cookieBlitPSO; ///< ライトクッキーのアトラスへの転写に使うパイプライン状態のハンドル。
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
    renderer::ResourceHandle<renderer::ShaderTag>           exposureHistogramCS; ///< 自動露出用の輝度ヒストグラムの集計に使うコンピュートシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag>           exposureAverageCS; ///< 輝度の集計からの露出算出に使うコンピュートシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ConstantBufferTag>   exposureCB; ///< 自動露出の算出に使う定数バッファのハンドル。
    /// @}

    /// @name 体積雲の作業 RT (ビュー単位)
    /// @{
    /// @note 雲はビューの解像度で描くので、static で共有すると SceneView と GameView が
    /// @note 1 フレーム内に寸法を取り合い、毎フレーム作り直しになる。所有は ViewRenderTargets。
    renderer::SizedRenderTarget* cloudRT      = nullptr; ///< 雲の体積散乱の評価に使う描画ターゲットとその寸法への非所有参照。
    renderer::SizedRenderTarget* cloudDepthRT = nullptr; ///< 雲の描画で参照する深度の記録に使う描画ターゲットとその寸法への非所有参照。
    /// @}

    /// @name 水面の屈折用コピー (ビュー単位)
    /// @{
    /// @note hdrRT を RTV として束縛したまま同じ色と深度を SRV で読めないので、水面を描く前に
    /// @note 別 RT へ写す。雲と同じ理由でビューが持つ (static だと 2 ビューで寸法を取り合う)。
    /// @note 水面が 1 つも見えないフレームでは確保しない。
    renderer::SizedRenderTarget* waterSceneColorRT = nullptr; ///< 水面の屈折で参照するシーン色の保持に使う描画ターゲットとその寸法への非所有参照。
    renderer::SizedRenderTarget* waterSceneDepthRT = nullptr; ///< 水面の下にあるシーン深度の保持に使う描画ターゲットとその寸法への非所有参照。
    /// @note Water Medium atlas and nearest visible water interface are view-owned graph targets.
    renderer::SizedRenderTarget* waterMediumAtlasRT = nullptr; ///< 水中の媒体情報のアトラスへの記録に使う描画ターゲットとその寸法への非所有参照。
    renderer::SizedRenderTarget* waterInterfaceRT = nullptr; ///< 水面境界の記録に使う描画ターゲットとその寸法への非所有参照。
    /// @note PS t14. The atlas texture is bound at t15 by passes that consume water-medium data.
    renderer::ResourceHandle<renderer::StructuredBufferTag> waterMediumTable;
    uint32_t waterMediumBodyCount = 0;
    uint32_t waterMediumTileResolution = 0;
    /// @note The table row and atlas tile share this body order for one immutable RenderScene snapshot.
    struct WaterMediumViewBody {
        uint32_t waterInputIndex = 0;
        uint32_t tileX = 0;
        uint32_t tileY = 0;
        math::Vector4 worldBoundsXZ{};
    };
    std::vector<WaterMediumViewBody> waterMediumBodies;
    /// @note Owners omitted by view resource limits fail closed in color, selection, and interface passes.
    std::vector<uint64_t> waterMediumDisabledOwnerKeys;
    /// @note Owners assigned a reduced atlas tile are listed for per-owner quality diagnostics.
    std::vector<uint64_t> waterMediumQualityLimitedOwnerKeys;
    /// @}

    /// @name パーティクル / コースティクスの作業 RT (ビュー単位)
    /// @{
    /// @note particleSceneColorRT は歪みパーティクルが屈折する «背景の退避»、
    /// @note particleOverdrawRT は重なり枚数の計数先、causticsDepthRT は深度のコピー。
    /// @note 3 つとも水面と同じ理由でビューが持つ。static のままだと SceneView と GameView が
    /// @note 1 枚を取り合い、寸法の違うフレームごとに作り直しが走る。
    renderer::SizedRenderTarget* particleSceneColorRT = nullptr; ///< 粒子の屈折で参照するシーン色の保持に使う描画ターゲットとその寸法への非所有参照。
    renderer::SizedRenderTarget* particleOverdrawRT   = nullptr; ///< 粒子の重なりの診断表示に使う描画ターゲットとその寸法への非所有参照。
    /// @note TAA の反応マスク。ParticleReactive パスが書き、TAA が t9 で読む。書いたフレームだけ valid。
    renderer::SizedRenderTarget* particleReactiveRT   = nullptr; ///< 粒子が時間方向の合成に与える影響の記録に使う描画ターゲットとその寸法への非所有参照。
    bool                         particleReactiveValid = false;
    renderer::SizedRenderTarget* causticsDepthRT      = nullptr; ///< 集光模様を受ける面の深度記録に使う描画ターゲットとその寸法への非所有参照。
    /// @}

    /// @name フロクセル ボリューメトリック フォグ
    /// @{
    /// @note froxelScatter は散乱と消散の生値、froxelIntegrated は Z 積分後の
    /// @note 「加算する光 (rgb) と背景の透過率 (a)」。Composite は後者だけを読む。
    renderer::ResourceHandle<renderer::TextureTag>        froxelScatter;
    renderer::ResourceHandle<renderer::TextureTag>        froxelScatterHistory;
    renderer::ResourceHandle<renderer::TextureTag>        froxelIntegrated;
    renderer::ResourceHandle<renderer::ShaderTag>         froxelInjectCS; ///< フロクセルごとの散乱光の計算に使うコンピュートシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag>         froxelIntegrateCS; ///< フロクセル列に沿った散乱光の積分に使うコンピュートシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ConstantBufferTag> froxelFogCB; ///< カメラのフロクセル格子への霧の設定に使う定数バッファのハンドル。

    /// @note コンピュートスキニング — ボーン変形を 1 フレーム 1 回だけ計算して各パスで共有する。
    renderer::ResourceHandle<renderer::ShaderTag>         skinningComputeCS; ///< ボーン変換による頂点の変形に使うコンピュートシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ConstantBufferTag> skinningCB; ///< ボーン変換による頂点の変形に使う定数バッファのハンドル。

    renderer::ResourceHandle<renderer::PipelineStateTag>  defaultPSO; ///< 通常のラスタ描画に使うパイプライン状態のハンドル。
    renderer::ResourceHandle<renderer::PipelineStateTag>  wireframePSO; ///< ワイヤーフレーム描画に使うパイプライン状態のハンドル。

    renderer::ResourceHandle<renderer::ShaderTag>         skyShader; ///< スカイドームの描画に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag>         skyCloudCaptureShader; ///< 環境マップに含める雲の描画に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag>         sunMoonShader; ///< 天空の太陽と月の描画に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::PipelineStateTag>  skyPSO; ///< スカイドームの描画に使うパイプライン状態のハンドル。
    renderer::ResourceHandle<renderer::PipelineStateTag>  sunMoonPSO; ///< 天空の太陽と月の描画に使うパイプライン状態のハンドル。
    renderer::ResourceHandle<renderer::BufferTag>         skyVB;
    renderer::ResourceHandle<renderer::BufferTag>         skyIB;
    uint32_t                                              skyIndexCount = 0;
    renderer::ResourceHandle<renderer::ConstantBufferTag> atmosphereCB; ///< 大気散乱の評価に使う定数バッファのハンドル。

    /// @note 空連動 IBL (環境システム Phase A): SkyCapture の描画先キューブマップと、
    /// @note 6 面それぞれの view/projection を渡す b0 用 CB (カメラ frameCB とは別に持つ)。
    renderer::ResourceHandle<renderer::RenderTargetTag>   skyEnvCubeRT; ///< 空の各方向のキューブマップへの描画に使う描画ターゲットのハンドル。
    renderer::ResourceHandle<renderer::ConstantBufferTag> skyCaptureFrameCB; ///< 環境光を撮影する視点の指定に使う定数バッファのハンドル。

    renderer::ResourceHandle<renderer::ShaderTag>         particleShader; ///< 粒子の描画に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::PipelineStateTag>  particlePSO; ///< 粒子の描画に使うパイプライン状態のハンドル。
    renderer::ResourceHandle<renderer::PipelineStateTag>  particleAlphaPSO; ///< アルファ合成による粒子描画に使うパイプライン状態のハンドル。
    renderer::ResourceHandle<renderer::PipelineStateTag>  particlePremultipliedPSO; ///< 乗算済みアルファでの粒子描画に使うパイプライン状態のハンドル。
    /// @note 頂点バッファは共有しない。エミッターごとに DynamicVertexBufferPool から借りる
    /// @note (共有 1 本だと DX12 で 2 個目以降の Update が 1 個目の Draw を壊す)。
    /// @note インデックスは全エミッター共通のクワッド列で、生成後は書き換えないので共有してよい。
    renderer::ResourceHandle<renderer::BufferTag>         particleIB;

    /// @note GPU パーティクル
    renderer::ResourceHandle<renderer::ShaderTag>         particleGpuSimCS;   ///< CS: シミュレーション+スポーン
    /// @note GPU ソート 3 段。半透明を大量に出すとき、描画順をカメラ距離で並べ替えるために使う。
    /// @note .cs.hlsl はエントリ 1 本なので、キー生成 / グローバル段 / LDS 段で 3 本に分かれる。
    renderer::ResourceHandle<renderer::ShaderTag>         particleGpuSortKeysCS; ///< GPU 粒子の並べ替えキーの生成に使うコンピュートシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag>         particleGpuSortStepCS; ///< GPU 粒子の並べ替えの各段に使うコンピュートシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag>         particleGpuSortLocalCS; ///< グループ内での GPU 粒子の並べ替えに使うコンピュートシェーダーのハンドル。
    /// @note メッシュパーティクルのインスタンス描画 (VS が SV_InstanceID で粒子を引く)。
    renderer::ResourceHandle<renderer::ShaderTag>         particleGpuMeshShader; ///< GPU 粒子のメッシュ描画に使うシェーダーのハンドル。
    /// @note 自己影: 光源から見た密度を積む専用 RT / シェーダー / 光源行列を入れた frame CB。
    /// @note 頂点展開ロジックを Particle.hlsl と共有するため b0 の view/viewProjection だけを
    /// @note 光源のものへ差し替える。h.frameCB を書き換えると後続パスへ漏れるため別 CB を持つ。
    renderer::ResourceHandle<renderer::RenderTargetTag>   particleSelfShadowRT; ///< 粒子同士による自己影の生成に使う描画ターゲットのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag>         particleSelfShadowShader; ///< 粒子同士による自己影の生成に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ConstantBufferTag> particleSelfShadowFrameCB; ///< 粒子自己影の投影情報の転送に使う定数バッファのハンドル。
    /// @note 自己影の密度バッファ 1 辺の解像度 [px]。
    /// @note 拾うのは「煙の内部で光がどれだけ減るか」という低周波の情報で輪郭の鮮鋭さは
    /// @note 要らないため、シャドウマップより粗くしてフィルレートを抑える。
    static constexpr std::uint32_t kSelfShadowResolution = 512u;
    /// @note VS+PS: billboard 描画。合成モードは PSO 側だけで切り替えるためシェーダーは 1 本。
    renderer::ResourceHandle<renderer::ShaderTag>         particleGpuShader; ///< GPU 粒子の描画に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::PipelineStateTag>  particleGpuPSO; ///< GPU 粒子の描画に使うパイプライン状態のハンドル。
    renderer::ResourceHandle<renderer::PipelineStateTag>  particleGpuAlphaPSO; ///< GPU 粒子のアルファ合成に使うパイプライン状態のハンドル。
    renderer::ResourceHandle<renderer::PipelineStateTag>  particleGpuPremultipliedPSO; ///< 乗算済みアルファでの GPU 粒子描画に使うパイプライン状態のハンドル。

    renderer::ResourceHandle<renderer::ShaderTag>         trailShader; ///< 連続する点列からなる軌跡の描画に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::PipelineStateTag>  trailPSO; ///< 連続する点列からなる軌跡の描画に使うパイプライン状態のハンドル。

    renderer::ResourceHandle<renderer::ShaderTag>         meshTrailShader; ///< メッシュを用いた軌跡の描画に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag>         skinnedMeshTrailShader; ///< スキニングしたメッシュ軌跡の描画に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::PipelineStateTag>  meshTrailPSO; ///< メッシュを用いた軌跡の描画に使うパイプライン状態のハンドル。
    renderer::ResourceHandle<renderer::PipelineStateTag>  meshTrailDoubleSidedPSO; ///< 両面を表示するメッシュ軌跡の描画に使うパイプライン状態のハンドル。

    renderer::ResourceHandle<renderer::ShaderTag>         gbufferShader; ///< 不透明面の G-buffer への記録に使うシェーダーのハンドル。
    /// @note gbufferShader のインスタンシング変種。
    /// @note 無効なら GBuffer パスは束ねずに 1 件ずつ出す (絵は変わらない)。
    /// @see Docs/design/gpu-instancing.md
    renderer::ResourceHandle<renderer::ShaderTag>         gbufferInstancedShader; ///< インスタンスをまとめた G-buffer 描画に使うシェーダーのハンドル。
    /// @note gbufferShader のスキンド変種。b7 のパレットで VS が変形する。
    /// @note コンピュートスキニングが効いていれば使わない (変形済みの頂点を素の GBuffer で描く)。
    /// @see Docs/design/pipeline-boundary.md §3
    renderer::ResourceHandle<renderer::ShaderTag>         gbufferSkinnedShader; ///< スキニングした形状の G-buffer 描画に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag>         deferredLightingShader; ///< G-buffer に記録した面のライティングに使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag>         depthCopyShader; ///< 後続パスが参照する深度のコピーに使うシェーダーのハンドル。
    /// @}

    /// @name クラスタライトカリング (Forward+ / Deferred+)
    /// @{
    /// @note punctualLightBuffer は PS の t29 / CS の t14 へ、clusterIndexBuffer は PS の t30 /
    /// @note CS の u2 へ束縛する。clusterCB (b9) はモードとグリッド係数を運ぶ。
    renderer::ResourceHandle<renderer::StructuredBufferTag> punctualLightBuffer;
    renderer::ResourceHandle<renderer::StructuredBufferTag> clusterIndexBuffer;
    renderer::ResourceHandle<renderer::ConstantBufferTag>   clusterCB; ///< クラスター照明のタイル・深度分割に使う定数バッファのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag>           clusterCullCS; ///< クラスターごとの局所光源の選別に使うコンピュートシェーダーのハンドル。

    /// @note clusterCB と同じ内容で、供給モードだけ Linear に固定した版。
    /// @note クラスタリストはメインカメラの視錐台に対して 1 回だけ作られるので、別視点の
    /// @note パス (リフレクションプローブ) が引くとまったく別の場所のライトを拾う。
    renderer::ResourceHandle<renderer::ConstantBufferTag>   clusterLinearCB; ///< 線形の深度分割を使うクラスター照明に使う定数バッファのハンドル。
    /// @}

    /// @name Advanced Graphics
    /// @{

    /// @note AdvancedGraphics 共用定数バッファ (b8)
    renderer::ResourceHandle<renderer::ConstantBufferTag> advancedGraphicsCB; ///< ビュー単位の描画効果の設定に使う定数バッファのハンドル。

    /// @note IBL (Image-Based Lighting)
    /// @note テクスチャハンドルは ResourceManager から取得した静的リソースで、
    /// @note シーンのスカイドームが変わるまで再ロード不要。
    renderer::ResourceHandle<renderer::TextureTag>        iblIrradiance;   ///< Diffuse irradiance cubemap
    renderer::ResourceHandle<renderer::TextureTag>        iblPrefilter;    ///< Specular prefiltered cubemap
    renderer::ResourceHandle<renderer::TextureTag>        iblBrdfLut;      ///< BRDF 積分 LUT (512x512 R16G16F)
    renderer::ResourceHandle<renderer::ShaderTag>         iblBrdfBakeShader; ///< CS: BRDF LUT をスタートアップ時に焼く
    renderer::ResourceHandle<renderer::RenderTargetTag>   iblBrdfLutRT;    ///< BRDF LUT bake 用 RT (静的)

    /// @note SSR (Screen Space Reflections)
    renderer::ResourceHandle<renderer::TextureTag>        ssrResult;       ///< SSR Compute 出力テクスチャ
    renderer::ResourceHandle<renderer::ShaderTag>         ssrShader;       ///< CS
    renderer::ResourceHandle<renderer::ShaderTag>         ssrResolveShader; ///< スクリーン空間の反射結果の解決に使うシェーダーのハンドル。
    /// @note a=1 は重み付け済み鏡面間接光、a=2 はガラスの全放射輝度。a=0 は従来反射へ戻す。
    renderer::ResourceHandle<renderer::TextureTag>        rayReflectionResult;

    /// @note Volumetric Lighting
    renderer::ResourceHandle<renderer::TextureTag>        volumetricResult;
    renderer::ResourceHandle<renderer::ShaderTag>         volumetricShader; ///< ビューに沿った体積光の描画に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag>         volumetricCloudShader; ///< 雲の体積散乱の評価に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag>         cloudUpscaleShader; ///< ハーフ解像度→HDR 合成
    renderer::ResourceHandle<renderer::PipelineStateTag>  volumetricCloudPSO; ///< 雲の体積散乱の評価に使うパイプライン状態のハンドル。
    renderer::ResourceHandle<renderer::PipelineStateTag>  volumetricCloudPremultipliedPSO; ///< 雲の premultiplied 合成専用
    renderer::ResourceHandle<renderer::ConstantBufferTag> volumetricCloudCB; ///< 雲の体積散乱の評価に使う定数バッファのハンドル。
    /// @note 3D ボリューメトリック雲ノイズ (起動時 CPU 焼き・タイラブル)。
    /// @note shape  = 128³ 低周波 Perlin-Worley + Worley FBM 帯 (RGBA)
    /// @note detail = 32³  高周波 Worley FBM (縁の侵食用)
    renderer::ResourceHandle<renderer::TextureTag>        cloudShapeTex;
    renderer::ResourceHandle<renderer::TextureTag>        cloudDetailTex;

    /// @note TAA (Temporal Anti-Aliasing)
    /// @note taaHistory は前フレームの TAA 出力を保持する永続 RT。解像度変更時のみ
    /// @note 再生成し、毎フレーム ping-pong で入れ替える。
    renderer::ResourceHandle<renderer::RenderTargetTag>   taaHistoryA;     ///< ping-pong バッファ A
    renderer::ResourceHandle<renderer::RenderTargetTag>   taaHistoryB;     ///< ping-pong バッファ B
    /// @note 各位相の未蓄積色と画素別の履歴有効長。既存の色・深度履歴と同じビュー寿命で保持する。
    renderer::ResourceHandle<renderer::RenderTargetTag>   taaRawHistory[8]{};
    renderer::ResourceHandle<renderer::ConstantBufferTag> taaCycleCB; ///< TAA のサンプリング位相の更新に使う定数バッファのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag>         taaShader;       ///< VS+PS
    renderer::ResourceHandle<renderer::PipelineStateTag>  taaPSO; ///< 履歴を用いた時間方向のアンチエイリアスに使うパイプライン状態のハンドル。
    /// @note A→B→A... の ping-pong フラグ。この構造体はフレームごとに作り直されるので、
    /// @note 正本はビュー側 (RenderSystem の ViewRenderTargets) が持ち、ここへは複写して渡す。
    bool                                                  taaFlip = false;

    /// @note Motion Blur
    renderer::ResourceHandle<renderer::TextureTag>        motionBlurResult;
    renderer::ResourceHandle<renderer::ShaderTag>         motionBlurShader; ///< 速度画像を使ったモーションブラーの合成に使うシェーダーのハンドル。

    /// @note GTAO (Ground Truth Ambient Occlusion)
    renderer::ResourceHandle<renderer::TextureTag>        gtaoRaw;
    renderer::ResourceHandle<renderer::ShaderTag>         gtaoShader; ///< 画面深度からの GTAO の算出に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag>         gtaoBlurShader; ///< GTAO で求めた遮蔽量の平滑化に使うシェーダーのハンドル。

    /// @note Contact Shadows
    renderer::ResourceHandle<renderer::ShaderTag>         contactShadowShader; ///< 深度を使ったスクリーン空間の接触影の計算に使うシェーダーのハンドル。

    /// @note Lens Flare
    renderer::ResourceHandle<renderer::ShaderTag>         lensFlareShader; ///< 光源のレンズフレア描画に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::PipelineStateTag>  lensFlarePSO;    ///< ADDITIVE ブレンド
    /// @note CPU生成32^3 RGBA8 LUT。外部DDSに依存せずCompositeのTexture3D(t22)へ束縛する。
    renderer::ResourceHandle<renderer::TextureTag>        proceduralColorLut;

    /// @name Light Probe Volume
    /// @{
    /// @note このフレームのライティングが引く SH ボリューム。[0] = 内側 (t22)、[1] = 外側 (t21)。無効ハンドルなら束縛しない。
    renderer::ResourceHandle<renderer::TextureTag>        lightProbeSH[2];
    renderer::ResourceHandle<renderer::TextureTag>        lightProbeVisibility[2];
    /// @note 6 面を SH へ射影する CS と、その定数 (b0)。
    renderer::ResourceHandle<renderer::ShaderTag>         lightProbeProjectCS; ///< キャプチャ結果の照明プローブへの投影に使うコンピュートシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ConstantBufferTag> lightProbeProjectCB; ///< キャプチャ結果の照明プローブへの投影に使う定数バッファのハンドル。
    /// @note 壁に埋まったプローブを周りで埋める CS と、その定数 (b0)。
    renderer::ResourceHandle<renderer::ShaderTag>         lightProbeDilateCS; ///< 照明プローブの値の近傍への拡張に使うコンピュートシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ConstantBufferTag> lightProbeDilateCB; ///< 照明プローブの値の近傍への拡張に使う定数バッファのハンドル。
    /// @note 面の表裏だけを書くシェーダー (カリング無し PSO で描く)。
    renderer::ResourceHandle<renderer::ShaderTag>         lightProbeFacingShader; ///< 照明プローブから見た面の向きの計測に使うシェーダーのハンドル。
    /// @note 捕捉の面ごとの b0。カメラの frameCB を上書きしないために分ける。
    renderer::ResourceHandle<renderer::ConstantBufferTag> lightProbeCaptureFrameCB; ///< 照明プローブのベイク視点の指定に使う定数バッファのハンドル。
    /// @note 捕捉で描くマテリアルが読む b8。画面空間 AO など «メインカメラの画面» に依存する項を切った写し。
    renderer::ResourceHandle<renderer::ConstantBufferTag> lightProbeCaptureAdvancedCB; ///< 照明プローブのベイク時の描画効果の設定に使う定数バッファのハンドル。
    /// @}
    /// @}
    /// @note View-owned normalized GBuffer draw snapshots; numeric handles from a different ResourceManager may collide.
    renderer::ResourceHandle<renderer::ConstantBufferTag> gbufferMaterialCB; ///< G-buffer に格納する材質の指定に使う定数バッファのハンドル。
    /// @note ReflectionProbe 捕捉専用。主ビューの b4/b12 を変更せず、camera-dependent shadow/cookie は unavailable。
    renderer::ResourceHandle<renderer::ConstantBufferTag> reflectionProbeCaptureShadowCB; ///< 反射プローブの撮影時の影の設定に使う定数バッファのハンドル。
    renderer::ResourceHandle<renderer::ConstantBufferTag> reflectionProbeCapturePunctualCB; ///< 反射プローブの撮影時の局所光源の設定に使う定数バッファのハンドル。
};

/// @note ShadowCascade — カスケード 1 枚ぶんの描画情報。RenderSystem が毎フレーム組み立て、
/// @note ShadowPass がアトラスのタイルへ描き、各ライティングパスが CB へ転送する。
/// @note 行列・カリング錐台・書き込み先タイル・バイアスを束ねてあるので、ShadowPass 側は
/// @note 「タイルを選んで既存の caster 提出を回す」だけで済む。
struct ShadowCascade {
    math::Matrix4 viewProjection; ///< ワールド座標をこのビューのクリップ座標に変換する行列。
    /// @note ライトビュー単体。第 3 行がライト前方への射影なので、caster を光源に近い順へ
    /// @note 並べ替えるための深度キー算出に使う (Hi-Z を効かせるための描画順)。
    math::Matrix4 view;
    /// @note ライト視点のワールド位置。
    math::Vector3 eyePos; ///< 光源ビューの原点のワールド座標。
    /// @note このカスケードの caster カリング用錐台 (viewProjection から抽出済み)。
    math::Frustum frustum;
    /// @note アトラス上の位置。xy = UV オフセット, zw = UV スケール (HLSL cascadeAtlasRect と同値)。
    math::Vector4 atlasRect; ///< アトラス上の描画矩形。xy が UV オフセット、zw が UV スケール。
    /// @note アトラス上のピクセル矩形。ShadowPass が IRenderer::SetViewport へ渡す。
    uint32_t      viewportX    = 0; ///< アトラス上の描画領域の左端（画素）。
    uint32_t      viewportY    = 0; ///< アトラス上の描画領域の上端（画素）。
    uint32_t      viewportSize = 0; ///< アトラス上の正方形の描画領域の一辺（画素）。
    /// @note このカスケードの正射影深度レンジで正規化した NDC バイアス。
    float         biasNDC = 0.0f; ///< 投影後の深度に加えるバイアス（NDC 単位）。
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
    math::Matrix4 viewProjection; ///< ワールド座標をこのビューのクリップ座標に変換する行列。
    /// @note ライトビュー単体。caster を光源に近い順へ並べるための深度キー算出に使う。
    math::Matrix4 view;
    /// @note ライト視点のワールド位置 (= ライトの位置)。
    math::Vector3 eyePos; ///< 光源ビューの原点のワールド座標。
    /// @note caster カリング用錐台 (viewProjection から抽出済み)。
    math::Frustum frustum;
    /// @note アトラス上の位置。xy = UV オフセット, zw = UV スケール。
    math::Vector4 atlasRect; ///< アトラス上の描画矩形。xy が UV オフセット、zw が UV スケール。
    /// @note アトラス上のピクセル矩形。ShadowPass が IRenderer::SetViewport へ渡す。
    uint32_t      viewportX    = 0; ///< アトラス上の描画領域の左端（画素）。
    uint32_t      viewportY    = 0; ///< アトラス上の描画領域の上端（画素）。
    uint32_t      viewportSize = 0; ///< アトラス上の正方形の描画領域の一辺（画素）。
    /// @note 透視投影の深度レンジで正規化した NDC バイアスと影の濃さ。
    float         biasNDC        = 0.0f; ///< 投影後の深度に加えるバイアス（NDC 単位）。
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
    math::Matrix4 viewProjection; ///< ワールド座標をこのビューのクリップ座標に変換する行列。
    /// @note アトラス上の位置。xy = UV オフセット, zw = UV スケール。
    math::Vector4 atlasRect; ///< アトラス上の描画矩形。xy が UV オフセット、zw が UV スケール。
    /// @note アトラス上のピクセル矩形の左上。サイズは kLightCookieTileSize 固定。
    uint32_t      viewportX = 0; ///< アトラス上の描画領域の左端（画素）。
    uint32_t      viewportY = 0; ///< アトラス上の描画領域の上端（画素）。
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
