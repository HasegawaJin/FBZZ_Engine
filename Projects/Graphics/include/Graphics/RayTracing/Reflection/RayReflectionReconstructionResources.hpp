/// @file    RayReflectionReconstructionResources.hpp
/// @brief   検証済み terminal 対応と境界検証付き反射再構成。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#pragma once
#include <Graphics/Api.hpp>
#include <Graphics/Renderer/Resources/ResourceHandle.hpp>
#include <Math/LinearAlgebra/Vector4.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fbzz::renderer {
struct RenderPassContext;
struct RenderViewResources;
struct RenderSharedResources;

/// @note Static-content keys prove unchanged terminal geometry/material; kind0 rejects moving reuse, kind1 is a reflected mesh terminal, kind2 is thin transmission with a known constant reflected environment.
/// @note terminalPositionParameter.w is zero for opaque and the first thin interface IOR for transmission.
struct RayReflectionMotionGuide {
    math::Vector4 terminalPositionParameter; ///< xyz は反射・透過先のワールド位置、w は薄板透過用の屈折率。
    std::array<uint32_t, 4> objectPrimitiveKind{}; ///< 終端形状を識別するオブジェクト添字・世代・三角形番号と w の終端分類。
};
static_assert(sizeof(RayReflectionMotionGuide) == 32);

/// @note owner index/generation、同内容版の dense material ID、有効度を保持する。Scene 世代はビューの exact 内容キーで検証する。
/// @note geometricNormalOffset.w は既存の法線方向 SpawnOffset。alpha0 は補完せず、glass は空間 filter を通さない。
struct RayReflectionSurface {
    math::Vector4 positionDepth; ///< xyz は受け手のワールド位置、w はカメラ前方の深さ [m]。
    math::Vector4 normalRoughness; ///< xyz は受け手のシェーディング法線、w は粗さ。
    math::Vector4 geometricNormalOffset; ///< xyz は受け手の幾何法線、w はレイ原点をずらす距離。
    std::array<uint32_t, 4> objectMaterialValid{}; ///< 受け手の Entity 添字・世代、密な材質添字と有効状態。
    RayReflectionMotionGuide motion; ///< 動くカメラで反射・透過先を照合するための終端情報。
};
static_assert(sizeof(RayReflectionSurface) == 96);

/// @note FP32 RGB 平均と有効重み。historyLimit までは算術平均、その後は重みを固定した bounded EMA。
/// @note count の負符号は薄板の透過入射信号、正符号は全 RAW/opaque 信号。絶対値が有効重み。
/// @note RAW Monte Carlo 和や完全な移動平均ではなく、Reference の蓄積へ混ぜない。
struct RayReflectionHistoryRecord {
    math::Vector4 radianceCount; ///< xyz は蓄積した RGB 平均、w は有効重みと信号種別を表す符号。
};
static_assert(sizeof(RayReflectionHistoryRecord) == 16);

/// @note b0。cameraPosition.w は orthographic0/1。動くカメラは実 terminal の対応を検証し、受け手 velocity を鏡面 motion に代用しない。
struct alignas(16) RayReflectionReconstructionConstants {
    math::Vector4 cameraPosition; ///< xyz は現在のカメラのワールド位置、w は平行投影のフラグ。
    math::Vector4 cameraRight; ///< 現在のカメラのワールド右方向と w の半投影幅。
    math::Vector4 cameraUp; ///< 現在のカメラのワールド上方向と w の半投影高。
    math::Vector4 cameraForward; ///< 現在のカメラのワールド前方方向。
    uint32_t width = 0; ///< 再構成するシーン出力の幅 [px]。
    uint32_t height = 0; ///< 再構成するシーン出力の高さ [px]。
    uint32_t resetHistory = 1; ///< 古い結合履歴を参照せずに蓄積を始めるか。
    uint32_t stage = 0; ///< 再構成ステージ。0 は時間方向、それ以外は空間方向。
    float previousJitterX = 0; ///< 確定した前フレームの射影ジッターの X 成分 [NDC]。
    float previousJitterY = 0; ///< 確定した前フレームの射影ジッターの Y 成分 [NDC]。
    float currentJitterX = 0; ///< 現在の射影ジッターの X 成分 [NDC]。
    float currentJitterY = 0; ///< 現在の射影ジッターの Y 成分 [NDC]。
    uint32_t historyLimit = 32; ///< 静止時に平均へ含める有効履歴重みの上限。
    uint32_t spatialRadius = 1; ///< 空間再構成で調べる近傍の半径 [px]。
    uint32_t hasGBuffer = 0; ///< このフレームの GBuffer で表面を照合できるか。
    uint32_t temporalAllowed = 1; ///< 内容版を照合でき、時間方向の履歴を使えるか。
    math::Vector4 previousCameraPosition; ///< xyz は確定した前カメラのワールド位置、w は平行投影フラグ。
    math::Vector4 previousCameraRight; ///< 確定した前カメラのワールド右方向と w の半投影幅。
    math::Vector4 previousCameraUp; ///< 確定した前カメラのワールド上方向と w の半投影高。
    math::Vector4 previousCameraForward; ///< 確定した前カメラのワールド前方方向。
    math::Vector4 constantEnvironmentRadiance; ///< xyz は定数環境の線形 RGB、w はその値を検証できたか。
    float nearDistance = 0; ///< 現在のカメラの近面距離 [m]。
    float farDistance = 0; ///< 現在のカメラの遠面距離 [m]。
    uint32_t cameraMotion = 0; ///< 確定済みフレームからカメラ入力が変わったか。
    uint32_t movingHistoryLimit = 4; ///< カメラ移動時に再利用する有効履歴重みの上限。
    /// @note 全解像度 guide の同一 surface/material 証明を通る粗い opaque セルだけ half RAW を復元する。
    uint32_t traceWidth = 0; ///< 生の反射トレース出力幅 [px]。
    uint32_t traceHeight = 0; ///< 生の反射トレース出力高 [px]。
    uint32_t resolutionDivisor = 1; ///< 生の反射トレースの解像度をシーン解像度から下げる除数。
    uint32_t reserved = 0; ///< 定数バッファの GPU 向け配置を保つ予約領域。
};
static_assert(sizeof(RayReflectionReconstructionConstants) == 224);
static_assert(offsetof(RayReflectionReconstructionConstants, traceWidth) == 208);
static_assert(offsetof(RayReflectionReconstructionConstants, traceHeight) == 212);
static_assert(offsetof(RayReflectionReconstructionConstants, resolutionDivisor) == 216);

/// @note Scene 画素ごとに Surface96×2 と RGB/count16×2 と RGBA16F output の232B、半解像度時は trace 画素ごとに half RAW8B を加算する。
/// @note trace metadata=u2、temporal t5 raw/t14 current/t15 previous/t18 previous history -> u2 current history、spatial t5/t14/t15 -> u0。両 stage は t16/17 GBuffer0/1 と半解像度時 t19 half RAW を読む。
/// @note 一つのビューが単独所有する。停止・失敗・非連続 frame・内容変化は履歴を無効にする。
/// @note Spatial 成功時だけ共通 index を進める。途中失敗後は部分更新も次フレームで破棄する。
struct RayReflectionReconstructionViewResources {
    std::array<ResourceHandle<StructuredBufferTag>, 2> surfaces{}; ///< 現在と前回の画素別反射ガイドを交互に保持するバッファ。
    std::array<ResourceHandle<StructuredBufferTag>, 2> histories{}; ///< 現在と前回の時間再構成結果を交互に保持するバッファ。
    ResourceHandle<TextureTag> output; ///< 時間・空間再構成の最終反射結果を受け取るテクスチャ。
    ResourceHandle<ConstantBufferTag> constants; ///< `constantsData` を再構成シェーダーへ渡すバッファ。
    RayReflectionReconstructionConstants constantsData; ///< 現在のカメラ・履歴設定を記録する CPU 側の定数。
    std::vector<uint32_t> contentKey; ///< 現在の表面、材質、照明などの入力が再利用可能か照合するキー。
    std::vector<uint32_t> cameraKey; ///< 前回からカメラ入力が変わったか比較するキー。
    RayReflectionReconstructionConstants committedCamera; ///< 最後に成功した空間再構成で確定したカメラ入力。
    uint32_t width = 0; ///< 確保済みのシーン側バッファ幅 [px]。
    uint32_t height = 0; ///< 確保済みのシーン側バッファ高 [px]。
    uint32_t surfaceReadIndex = 0; ///< 前回確定した `surfaces` と `histories` を読む共通添字。
    uint64_t lastFrameStamp = 0; ///< 空間再構成を最後に確定したフレーム番号。
    float previousJitterX = 0; ///< 最後に確定したカメラジッターの X 成分 [NDC]。
    float previousJitterY = 0; ///< 最後に確定したカメラジッターの Y 成分 [NDC]。
    bool prepared = false; ///< このフレームの再構成資源と定数を準備できたか。
    bool historyValid = false; ///< 前回確定した時間方向履歴が利用可能か。
    bool rawSucceeded = false; ///< 現在フレームの生反射トレースが成功したか。
    bool temporalSucceeded = false; ///< 現在フレームの時間再構成が成功したか。
};

/// @return false は再構成だけを無効化し、同フレームの生 Reflection を維持する。
/// @note 実験 RT のセッション許可なしでは専用 shader / GPU 資源を生成せず false。
[[nodiscard]] FBZZ_GRAPHICS_API bool PrepareRayReflectionReconstruction(RenderPassContext& context,
    RenderViewResources& view, RenderSharedResources& shared);
} // namespace fbzz::renderer
