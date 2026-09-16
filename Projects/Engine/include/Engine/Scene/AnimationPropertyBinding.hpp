/// @file    AnimationPropertyBinding.hpp
/// @brief   PropertyAnimationTrack を Component / Material / Script のプロパティへ結ぶ
/// @author  Hasegawa Jin
/// @date    2026-08-26
#pragma once

#include <Engine/Asset/AnimationClip.hpp>

#include <cstdint>
#include <vector>

// WHY .anim と .sequence で共有するか:
//   どちらも「componentType + propertyName で Reflect() の 1 フィールドを探して書く」
//   という同じ解決規則に乗る。実装が 2 つあると、片方だけが Script のフィールドを
//   見る / 見ないといった差が出て、同じ track を Animator で見たときと演出で見たときで
//   当たり先が変わる。

namespace fbzz::scene {

class GameObject;

/// スナップショット 1 件。停止時に元の値へ戻すために持つ。
struct AnimationPropertySample {
    asset::AnimValueType type = asset::AnimValueType::Float;
    float                floats[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    int32_t              intValue  = 0;
    bool                 boolValue = false;
    /// 一致するフィールドが見つかったか。false のスナップショットは復帰に使わない。
    bool                 found = false;
};

/// ticks 時点の値を Component / Script のプロパティへ書き込む。
void ApplyComponentProperty(GameObject& target,
                            const asset::PropertyAnimationTrack& track,
                            double ticks);

/// ticks 時点の値を MaterialComponent の paramOverrides へ書き込む。
void ApplyMaterialProperty(GameObject& target,
                           const asset::PropertyAnimationTrack& track,
                           double ticks);

/// 現在値を読み出す。track.valueType の型で一致するフィールドだけを見る。
[[nodiscard]] AnimationPropertySample CaptureComponentProperty(
    GameObject& target, const asset::PropertyAnimationTrack& track);

/// CaptureComponentProperty で取った値を書き戻す。
void RestoreComponentProperty(GameObject& target,
                              const asset::PropertyAnimationTrack& track,
                              const AnimationPropertySample& sample);

/// paramOverrides に該当キーがあれば true を返し、その値を outValues へ複写する。
[[nodiscard]] bool CaptureMaterialProperty(GameObject& target,
                                           const asset::PropertyAnimationTrack& track,
                                           std::vector<float>& outValues);

/// hadOverride が false なら該当キーを消す (元は override が無かったということ)。
void RestoreMaterialProperty(GameObject& target,
                             const asset::PropertyAnimationTrack& track,
                             const std::vector<float>& values,
                             bool hadOverride);

} // namespace fbzz::scene
