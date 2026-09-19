/// @file    AnimationPropertyBinding.hpp
/// @brief   PropertyAnimationTrack を Component / Material / Script のプロパティへ結ぶ
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// @note .anim と .sequence は「componentType + propertyName で Reflect() の1フィールドを探して書く」同じ解決規則を共有する。実装を分けると当たり先がずれる。
#pragma once

#include <Engine/Asset/AnimationClip.hpp>

#include <cstdint>
#include <vector>

namespace fbzz::scene {

class GameObject;

/// @brief スナップショット 1 件。停止時に元の値へ戻すために持つ。
struct AnimationPropertySample {
    asset::AnimValueType type = asset::AnimValueType::Float;
    float                floats[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    int32_t              intValue  = 0;
    bool                 boolValue = false;
    bool                 found = false; ///< 一致するフィールドが見つかったか。false のスナップショットは復帰に使わない。
};

/// @brief ticks 時点の値を Component / Script のプロパティへ書き込む。
void ApplyComponentProperty(GameObject& target,
                            const asset::PropertyAnimationTrack& track,
                            double ticks);

/// @brief ticks 時点の値を MaterialComponent の paramOverrides へ書き込む。
void ApplyMaterialProperty(GameObject& target,
                           const asset::PropertyAnimationTrack& track,
                           double ticks);

/// @brief 現在値を読み出す。track.valueType の型で一致するフィールドだけを見る。
[[nodiscard]] AnimationPropertySample CaptureComponentProperty(
    GameObject& target, const asset::PropertyAnimationTrack& track);

/// @brief CaptureComponentProperty で取った値を書き戻す。
void RestoreComponentProperty(GameObject& target,
                              const asset::PropertyAnimationTrack& track,
                              const AnimationPropertySample& sample);

/// @brief paramOverrides に該当キーがあれば true を返し、その値を outValues へ複写する。
[[nodiscard]] bool CaptureMaterialProperty(GameObject& target,
                                           const asset::PropertyAnimationTrack& track,
                                           std::vector<float>& outValues);

/// @brief hadOverride が false なら該当キーを消す (元は override が無かったということ)。
void RestoreMaterialProperty(GameObject& target,
                             const asset::PropertyAnimationTrack& track,
                             const std::vector<float>& values,
                             bool hadOverride);

} // namespace fbzz::scene
