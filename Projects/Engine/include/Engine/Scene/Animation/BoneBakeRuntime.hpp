/// @file    BoneBakeRuntime.hpp
/// @brief   ベイク済み骨アニメーションの評価・保持・CPU 姿勢復元。
/// @author  Hasegawa Jin
/// @date    2026-10-12
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Scene/Components/Animation/BoneBakeTypes.hpp>
#include <span>

namespace fbzz::renderer { class ResourceManager; }
namespace fbzz::asset { struct Model; struct AnimationClip; }
namespace fbzz::scene {
struct AnimatorComponent;

struct BoneBakeClipInput {
    const asset::AnimationClip* clip = nullptr;
    double ticks = 0.0;
    float weight = 0.0f;
};

/// @note 再生前のアセット読み込み・編集準備で共有派生キャッシュを用意する。非対応のクリップは Animator の通常入力として扱う。
FBZZ_ENGINE_API void PrepareBoneBakeAssets(AnimatorComponent& animator, const asset::Model& model);

/// @note 準備済みの不変キャッシュだけを使う。入力元、サンプルレート、世代が変わった場合や時計が無効な場合は姿勢を作り直す。
/// @note 公開姿勢を保持する前に、呼び出し側で距離 LOD を適用し、CPU ボーン利用側、制御、遷移を除外する。
FBZZ_ENGINE_API bool CanHoldBoneBake(const AnimatorComponent& animator, const asset::Model& model,
                     std::span<const BoneBakeClipInput> clips);

/// @note この経路の呼び出し側は、CPU ボーン利用側、リグ制御、マスク、レイヤー、モーフ、ルートモーションを除外する。
/// @note GPU_POSE_SHARED は GPU 準備成功後だけ CPU パレットを保留する。CPU ボーンを読む処理では使わない。
/// @note 従来の利用側に渡す前、または GPU の準備失敗時に、保留した現在と前回の CPU パレットを再構築する。
/// @return false のときは前回の履歴を無効にする。検証済みの不変ベイク入力は通常評価に失敗しない。
FBZZ_ENGINE_API bool MaterializeBoneBakeCpu(AnimatorComponent& animator, renderer::ResourceManager& resources, bool current);

FBZZ_ENGINE_API bool TryEvaluateBoneBake(AnimatorComponent& animator, const asset::Model& model,
                         std::span<const BoneBakeClipInput> clips);
}
