/// @file    BoneBakeTypes.hpp
/// @brief   Animator が保持するベイク姿勢・状態の値型。評価処理には依存しない。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Asset/Animation/Clips/BakedBoneAnimation.hpp>
#include <Graphics/Renderer/Resources/ResourceHandle.hpp>
#include <cstdint>
#include <memory>

namespace fbzz::scene {
enum class BoneBakeMode : int { LEGACY = 0, CPU_BAKED = 1, GPU_SHARED = 2, GPU_POSE_SHARED = 3 };
enum class BoneBakeFallback : uint8_t {
    NONE, DISABLED, NEAR_CAMERA, CPU_BONES_REQUIRED, RIG_CONTROL,
    ROOT_MOTION, LAYERS_OR_MASK, MORPH, MUTABLE_CLIP, CLIP_COUNT,
    INVALID_SOURCE, INVALID_CLOCK, INCOMPATIBLE_LOD, GPU_RESOURCE, GPU_DISPATCH
};
FBZZ_ENGINE_API const char* BoneBakeFallbackName(BoneBakeFallback reason);

/// @note 不変のクリップ参照により、LOD で姿勢を保持する間やクリップ遷移をまたいでも直前の表示姿勢を復元できる。
struct BoneBakePoseState {
    std::shared_ptr<const asset::BakedBoneAnimation> source; ///< 保存した姿勢の遷移元を復元する不変ベイクデータの共有参照。
    std::shared_ptr<const asset::BakedBoneAnimation> target; ///< 保存した姿勢の遷移先を復元する不変ベイクデータの共有参照。
    asset::BakedBoneFrame frame; ///< 保存時点で遷移元から選んだサンプルフレーム。
    asset::BakedBoneFrame targetFrame; ///< 保存時点で遷移先から選んだサンプルフレーム。
    float blendWeight = 0.0f; ///< 保存した姿勢を復元するときに遷移先へ掛ける混合率。
};
struct BoneBakeRuntime {
    BoneBakePoseState previous; ///< 現姿勢を選ぶ前に保存した直前のベイク姿勢。
    renderer::ResourceHandle<renderer::StructuredBufferTag> gpuPalette; ///< 現在の姿勢のスキニングに使う GPU パレットの非所有ハンドル。
    renderer::ResourceHandle<renderer::StructuredBufferTag> gpuPreviousPalette; ///< 前の姿勢のスキニングに使う GPU パレットの非所有ハンドル。
    uint32_t gpuOffset = 0; ///< 現在のパレットが始まる GPU バッファ内の要素オフセット。
    uint32_t gpuPreviousOffset = 0; ///< 前回のパレットが始まる GPU バッファ内の要素オフセット。
    uint64_t gpuOwner = 0; ///< パレットの所有元を照合する識別値。
    uint64_t gpuEpoch = 0; ///< 再確保された GPU パレットを区別する世代識別値。
    uint64_t gpuFrame = UINT64_MAX; ///< GPU パレットを割り当てたフレーム番号。未割り当てなら UINT64_MAX。
    bool previousPoseValid = false; ///< 速度などの計算に使える前回の姿勢を保持しているかどうか。
    bool gpuReady = false; ///< 現在の描画で GPU パレットを使えるかどうか。
    bool cpuDeferred = false; ///< 現在の姿勢の CPU 評価を保留しているかどうか。
    bool previousCpuDeferred = false; ///< 前の姿勢の CPU 評価を保留したかどうか。
    bool active = false; ///< 今回の姿勢にベイク済みアニメーションを使用しているかどうか。
    bool invalidateHistory = true; ///< 次に姿勢を更新するときに前回の描画履歴を引き継いではいけないかどうか。
    BoneBakeFallback fallback = BoneBakeFallback::DISABLED; ///< 最新の評価でベイク再生から通常評価へ戻った理由。
    BoneBakeFallback reportedFallback = BoneBakeFallback::DISABLED; ///< 同じベイク再生の失敗を繰り返し通知しないために保持する報告済み理由。
    asset::BoneBakeStatus bakeStatus = asset::BoneBakeStatus::OK; ///< ベイク済み姿勢を準備したときの結果コード。
    int generation = -1; ///< 現在のベイク済みデータが対応するアセット世代。
    float sampleRate = 0.0f; ///< 使用中のベイクアニメーションのサンプル周波数 [Hz]。
    float radius = 0.0f; ///< ベイク再生でカリングに使う姿勢の境界球半径 [m]。
    std::shared_ptr<const asset::BakedBoneAnimation> source; ///< 現在の遷移元の不変ベイクデータの共有参照。
    std::shared_ptr<const asset::BakedBoneAnimation> target; ///< 現在の遷移先の不変ベイクデータの共有参照。
    asset::BakedBoneFrame frame; ///< 遷移元の現在の再生位置に対応するサンプルフレーム。
    asset::BakedBoneFrame targetFrame; ///< 遷移先の現在の再生位置に対応するサンプルフレーム。
    float blendWeight = 0.0f; ///< 現在の姿勢を求めるときに遷移先へ掛ける混合率。
    asset::BakedBoneWorkspace workspace; ///< ベイク姿勢の CPU 評価で使い回す一時領域。
};
}
