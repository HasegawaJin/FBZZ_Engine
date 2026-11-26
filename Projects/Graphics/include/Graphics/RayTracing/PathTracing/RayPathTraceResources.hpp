/// @file    RayPathTraceResources.hpp
/// @brief   Reference Path の生 FP32 蓄積とビュー表面出力。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#pragma once
#include <Graphics/Api.hpp>
#include <Graphics/RayTracing/Scene/RayGeometryCache.hpp>
#include <Graphics/RayTracing/PathTracing/RayPathHistory.hpp>
#include <Graphics/RayTracing/PathTracing/RayPathScene.hpp>
#include <Graphics/RayTracing/Scene/RayEnvironment.hpp>
#include <Graphics/RayTracing/Scene/RayGameResources.hpp>

namespace fbzz::renderer {

/// @note GPU RAW は露出前の FP32 和と整数 sample count。表示用 RGBA16F を履歴へ戻さない。
/// @note UINT32_MAX は GPU failure の sticky 診断であり平均を持たない。内容 key の reset だけで解除する。
struct RayPathHistoryRecord {
    math::Vector3 radianceSum; ///< 画素で蓄積した露出前のシーン線形 RGB 放射輝度の和。
    uint32_t sampleCount = 0; ///< その和に含まれるサンプル数。`UINT32_MAX` はリセットまで残る GPU エラー。
};
static_assert(sizeof(RayPathHistoryRecord) == 16);

/// @note materialId は immutable surface table の dense instance 添字。valid=0 は背景、UINT32_MAX は sticky GPU 診断。
struct RayPathIdRecord {
    uint32_t objectIndex = 0; ///< 中心レイの先頭ヒットを識別する Entity ID の添字部。
    uint32_t objectGeneration = 0; ///< 中心レイの先頭ヒットを識別する Entity ID の世代部。
    uint32_t materialId = 0; ///< 先頭ヒットの材質を引く、密なインスタンス表の添字。
    uint32_t valid = 0; ///< 先頭ヒットの有効状態。0 は背景、`UINT32_MAX` は GPU 診断。
};
static_assert(sizeof(RayPathIdRecord) == 16);

/// @note b0=208 bytes。environmentMode: 0=black、1=明示定数、2=生 cube (t4)。shape=t6、environment CDF=t7。
/// @note finite bounce 上限は明示した切り詰め。clamp/denoise/SSR/probe/ambient 近似を RAW に適用しない。
struct RayPathTraceConstants {
    math::Vector4 cameraPosition; ///< カメラ原点のワールド位置を渡すベクトル。
    math::Vector4 cameraRight; ///< ワールド右方向と w の半投影幅。
    math::Vector4 cameraUp; ///< ワールド上方向と w の半投影高。
    math::Vector4 cameraForward; ///< レイの基準となるカメラのワールド前方ベクトル。
    math::Vector4 lightDirection; ///< 単一方向光を使う場合の光のワールド方向。
    math::Vector4 lightRadiance; ///< 単一方向光のシーン線形 RGB 放射輝度。
    math::Vector4 environmentRadiance; ///< 定数環境を使う場合のシーン線形 RGB 放射輝度。
    uint32_t width = 0; ///< パストレース出力の幅 [px]。
    uint32_t height = 0; ///< パストレース出力の高さ [px]。
    uint32_t instanceCount = 0; ///< GPU ヒット表で参照できるインスタンス数。
    uint32_t emitterCount = 0; ///< 発光三角形のサンプリング表のレコード数。
    float nearDistance = 0; ///< カメラ近面までの距離 [m]。
    float farDistance = 0; ///< カメラ遠面までの距離 [m]。
    uint32_t orthographic = 0; ///< 平行投影で画素ごとのレイ原点を移動するか。
    uint32_t environmentMode = 0; ///< 環境の取得方式。0 は黒、1 は定数、2 は生キューブ。
    uint32_t maxBounces = 8; ///< 1 パスに許可する散乱反復数の上限。
    uint32_t samplesPerDispatch = 1; ///< この dispatch で画素ごとに生成する新規サンプル数。
    uint32_t sampleBase = 0; ///< 今回の蓄積で最初に使うサンプル番号。
    uint32_t resetHistory = 1; ///< 以前の放射輝度の和とサンプル数を破棄するか。
    uint32_t rouletteStart = 3; ///< ロシアンルーレットを使い始める散乱反復番号。
    uint32_t enableNee = 1; ///< 直接光を次事象推定でサンプリングするか。
    uint32_t enableMis = 1; ///< 光源と BSDF のサンプリングを MIS で重み付けするか。
    uint32_t samplerSeed = 0; ///< サンプル系列を選択する疑似乱数シード。
    uint32_t deltaLightCount = 0; ///< 点・スポット・方向光の表のレコード数。
    uint32_t shapeLightCount = 0; ///< 球・カプセル光源の表のレコード数。
    std::array<uint32_t, 2> padding{}; ///< 定数バッファの GPU 向け配置を保つ予約領域。
    uint32_t environmentTableCount = 0; ///< 環境方向のサンプリング分布のレコード数。
    uint32_t environmentFaceSize = 0; ///< 環境方向のサンプリング表の 1 面の幅 [texel]。
    float environmentRotation = 0; ///< 環境キューブのワールド Y 軸回転 [rad]。
    float environmentIntensity = 1; ///< 生の環境放射輝度に掛ける倍率。
};
static_assert(sizeof(RayPathTraceConstants) == 208);

/// @note auxiliary surface は center ray。radiance AA のサンプル位置や frameStamp で履歴をリセットしない。
struct RayPathViewResources {
    ResourceHandle<TextureTag> output; ///< パストレースの表示用放射輝度を受け取るテクスチャ。
    ResourceHandle<TextureTag> firstSurface; ///< 中心レイの先頭ヒットの表面情報を出力するテクスチャ。
    ResourceHandle<TextureTag> firstMaterial; ///< 中心レイの先頭ヒットの材質情報を出力するテクスチャ。
    ResourceHandle<TextureTag> firstGeometry; ///< 中心レイの先頭ヒットの幾何情報を出力するテクスチャ。
    ResourceHandle<StructuredBufferTag> historyBuffer; ///< 露出前の FP32 放射輝度の和と整数サンプル数を保持するバッファ。
    ResourceHandle<StructuredBufferTag> idsBuffer; ///< 中心レイのヒット元と材質を識別する画素別バッファ。
    ResourceHandle<ConstantBufferTag> constants; ///< `constantsData` をパストレースシェーダーへ渡す定数バッファ。
    ResourceHandle<StructuredBufferTag> emitters; ///< GPU が直接サンプリングする発光三角形のバッファ。
    ResourceHandle<StructuredBufferTag> deltaLights; ///< GPU が読む点・スポット・方向光のバッファ。
    ResourceHandle<StructuredBufferTag> shapes; ///< GPU が直接サンプリングする球・カプセル光源のバッファ。
    ResourceHandle<StructuredBufferTag> environmentTable; ///< 環境方向を重点サンプリングするための GPU 分布表。
    ResourceHandle<TextureTag> environment; ///< シェーダーが放射輝度を読む生の環境キューブのハンドル。
    RayEnvironmentDistribution environmentDistribution; ///< 環境の GPU 分布表を作った CPU レコードと内容版。
    uint32_t width = 0; ///< このビューで確保した出力幅 [px]。
    uint32_t height = 0; ///< このビューで確保した出力高 [px]。
    RayScene scene; ///< TLAS と表を作る元となった CPU レイシーン。
    RaySceneGpu gpu; ///< `scene` に対応する TLAS とヒット・材質表の GPU 公開結果。
    RayPathSceneBuilder sceneBuilder; ///< 積分用の形状読み出しと内容版を保持する、このビューのビルダー。
    RayPathScene pathScene; ///< このビューで積分する光源・環境と発光形状の CPU スナップショット。
    uint64_t emitterRevision = 0; ///< GPU の光源表に反映したパスシーンの内容版。
    RayPathTraceConstants constantsData; ///< シェーダーへ転送するカメラ、積分器、光源の定数。
    RayPathHistory history; ///< Reference で蓄積済みサンプルの整合性と件数を管理する履歴。
    bool dispatchSucceeded = false; ///< 今回のトレース dispatch を成功として処理できたか。
    RayGameViewResources game; ///< Game プロファイルの輸送と履歴再構成の資源。
    bool gameProfile = false; ///< このビューが Game Path の処理を使うか。
};

/// @return カメラ契約が無効なら false。カメラ以外の integrator 設定は変更しない。
[[nodiscard]] FBZZ_GRAPHICS_API bool MakeRayPathCameraConstants(const Camera& camera, RayPathTraceConstants& output);

} // namespace fbzz::renderer
