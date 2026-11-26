/// @file    RayGameResources.hpp
/// @brief   Game Path の新規輸送成分とビュー別ノイズ再構成履歴。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#pragma once
#include <Graphics/Api.hpp>
#include <Graphics/Renderer/View/Camera.hpp>
#include <Graphics/Renderer/Resources/ResourceHandle.hpp>
#include <Math/LinearAlgebra/Vector4.hpp>
#include <array>
#include <cstdint>
#include <vector>

namespace fbzz::renderer {

/// @note 生 FP32 成分。first receiver の full BSDF 比で配分し、選択した lobe のタグでは分離しない。
/// @see https://pbr-book.org/4ed/Light_Transport_I_Surface_Reflection/A_Better_Path_Tracer Full BSDF throughput
struct RayGameTransportRecord {
    math::Vector4 diffuse; ///< 先頭表面の BSDF の拡散成分に配分した生の RGB 輸送。
    math::Vector4 specular; ///< 先頭表面の BSDF の鏡面成分に配分した生の RGB 輸送。
    math::Vector4 independent; ///< 先頭表面の拡散・鏡面に配分しなかった生の RGB 輸送。
};
static_assert(sizeof(RayGameTransportRecord) == 48);

/// @note valid: 1=Raster 照合済み、2=背景、3=中心境界の recast 補完、UINT_MAX=輸送失敗。
/// @note sceneGeneration を含む完全 ID。materialId は同じ view 内容版の dense instance 添字。
struct RayReconstructionSurface {
    math::Vector4 positionDepth; ///< xyz は先頭ヒットのワールド位置、w はカメラから得た正規化深度。
    math::Vector4 normalRoughness; ///< xyz はシェーディング法線、w は粗さ。
    math::Vector4 albedoMetallic; ///< xyz は線形 RGB の基準色、w は金属度。
    math::Vector4 geometricNormalHitDistance; ///< xyz は幾何法線、w は受け手に達したレイ区間の距離。
    math::Vector4 previousPositionValid; ///< xyz は再投影に使う前フレームのワールド位置、w はその有効状態。
    std::array<uint32_t, 4> objectMaterialValid{}; ///< Entity 添字、Entity 世代、密な材質添字、および表面一致の有効状態。
    std::array<uint32_t, 4> sceneFlags{}; ///< 完全なオブジェクト ID を照合する Scene 世代の下位・上位のワードを含む。
};
static_assert(sizeof(RayReconstructionSurface) == 112);

/// @note diffuse.w は有効履歴長。specular は hit motion 未供給の初期契約で時間方向へ再利用しない。
struct RayReconstructionHistoryRecord {
    math::Vector4 diffuse; ///< xyz は再構成した拡散 RGB、w は有効履歴の長さ。
    math::Vector4 specular; ///< 再構成が参照する鏡面 RGB。時間方向には持ち越さない。
    RayReconstructionSurface surface; ///< この履歴と同じ画素に対応する表面の照合情報。
};
static_assert(sizeof(RayReconstructionHistoryRecord) == 144);

/// @note CPU Matrix4 と同じ row-major storage。GPU は column vector で変換する。
struct RayGameMotionRecord {
    math::Matrix4 currentWorldInverse = math::Matrix4::Identity(); ///< 現在フレームのワールド位置を形状ローカル空間へ戻す変換。
    math::Matrix4 previousWorld = math::Matrix4::Identity(); ///< 前フレームの形状ローカル空間からワールド空間への変換。
    uint32_t temporalValid = 0; ///< 現在のヒットから前フレームの位置を求められるか。
    std::array<uint32_t, 3> reserved{}; ///< GPU レコードの配置を保つ予約領域。
};
static_assert(sizeof(RayGameMotionRecord) == 144);

/// @note b1。未束縛の Reference は gameMode=0 として既存 b0 と UAV layout を保つ。
struct alignas(16) RayGameTraceConstants {
    uint32_t gameMode = 1; ///< Game Path の輸送出力を有効にするか。
    uint32_t frameSampleIndex = 0; ///< 今回の dispatch のサンプル系列を選ぶ番号。
    std::array<uint32_t, 2> reserved{}; ///< 定数バッファの GPU 向け配置を保つ予約領域。
};

/// @note previous camera basis で再投影し、逆 VP の far/ortho 特異点に依存しない。
struct alignas(16) RayReconstructionConstants {
    math::Vector4 previousCameraPosition; ///< 再投影先となる前カメラのワールド原点。
    math::Vector4 previousCameraRight; ///< 前カメラのワールド右方向と w の半投影幅。
    math::Vector4 previousCameraUp; ///< 前カメラのワールド上方向と w の半投影高。
    math::Vector4 previousCameraForward; ///< 前カメラのワールド前方方向。
    uint32_t width = 0; ///< 再構成対象の幅 [px]。
    uint32_t height = 0; ///< 再構成対象の高さ [px]。
    uint32_t previousOrthographic = 0; ///< 前カメラが平行投影だったか。
    uint32_t resetHistory = 1; ///< 古い履歴を参照せずに再構成を始めるか。
    uint32_t stage = 0; ///< 再構成ステージ。0 は時間方向、それ以外は空間方向。
    uint32_t diffuseHistoryLimit = 8; ///< 拡散成分で再利用する有効履歴長の上限。
    uint32_t spatialRadius = 1; ///< 空間再構成で参照する近傍の半径 [px]。
    uint32_t reserved = 0; ///< 定数バッファの GPU 向け配置を保つ予約領域。
};
static_assert(sizeof(RayReconstructionConstants) == 96);

/// @note initial cut: translation >1m、forward rotation >45deg、射影入力の変更。通常の動きは画素照合へ渡す。
[[nodiscard]] FBZZ_GRAPHICS_API bool IsRayGameCameraCut(const Camera& previous, const Camera& current);
/// @note ID/material/position/normal の検証を共有する CPU 契約。履歴に未計算の黒を観測値として入れない。
[[nodiscard]] FBZZ_GRAPHICS_API bool IsRayReconstructionHistoryCompatible(
    const RayReconstructionSurface& current, const RayReconstructionSurface& previous, const Camera& previousCamera);

/// @note Reference RAW と独立して view が所有する。frameSampleIndex は成功 dispatch 後だけ進める。
struct RayGameViewResources {
    ResourceHandle<StructuredBufferTag> transport; ///< 画素別の拡散・鏡面・独立の生輸送成分を受け取るバッファ。
    ResourceHandle<StructuredBufferTag> surface; ///< 現在フレームの再構成が参照する画素別の先頭表面バッファ。
    std::array<ResourceHandle<StructuredBufferTag>, 2> reconstructionHistory{}; ///< 再構成履歴を現在と前回で交互に読み書きする GPU バッファ。
    ResourceHandle<StructuredBufferTag> motionInstances; ///< 密なインスタンス添字で参照する再投影用変換バッファ。
    ResourceHandle<ConstantBufferTag> traceConstants; ///< `traceData` を Game Path シェーダーへ渡すバッファ。
    ResourceHandle<ConstantBufferTag> reconstructionConstants; ///< `reconstructionData` を再構成シェーダーへ渡すバッファ。
    RayGameTraceConstants traceData; ///< Game 輸送の有効状態とサンプル番号を渡す CPU 側定数。
    RayReconstructionConstants reconstructionData; ///< 前カメラと履歴の条件を渡す CPU 側の再構成定数。
    Camera previousCamera; ///< 最後に再構成履歴を確定したカメラ。
    std::vector<uint32_t> contentKey; ///< 形状・材質・照明が同じ履歴を再利用できるか照合するキー。
    uint32_t frameSampleIndex = 0; ///< 成功したトレース dispatch の後で進めるサンプル系列番号。
    uint32_t historyReadIndex = 0; ///< 最後に確定した `reconstructionHistory` を読む添字。
    uint64_t lastFrameStamp = 0; ///< 再構成履歴を最後に確定した描画フレーム番号。
    bool prepared = false; ///< このフレームの Game Path 用資源を準備できたか。
    bool historyValid = false; ///< 確定済みの時間方向履歴を照合に使えるか。
    bool reconstructed = false; ///< このフレームの空間再構成結果を出力できたか。
    bool temporalSucceeded = false; ///< このフレームの時間方向再構成が成功したか。
};

} // namespace fbzz::renderer
