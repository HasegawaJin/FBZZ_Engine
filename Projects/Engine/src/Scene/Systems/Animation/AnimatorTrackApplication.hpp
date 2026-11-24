/// @file    AnimatorTrackApplication.hpp
/// @brief   Animator からクリップの対象解決、トラック適用とイベントを呼ぶ内部境界。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once
#include <string_view>

namespace fbzz::asset {
struct AnimationClip;
}

namespace fbzz::scene {
class GameObject;
struct AnimatorComponent;
struct SkinnedMeshRenderer;

namespace animator_detail {

/// @return 相対パスに一致する最初のオブジェクト。空や "." は root、見つからなければ nullptr。
GameObject* FindAnimationTarget(GameObject& root, std::string_view path);

/// @note 旧クリップの未指定パスをボーン名で補う。装飾オブジェクトは対象に含めない。
GameObject* FindAnimationTargetByNodeName(GameObject& root, std::string_view nodeName);

void ApplyTransformTracks(GameObject& root, const asset::AnimationClip& clip, double ticks);
void ApplyPropertyTracks(GameObject& root, const asset::AnimationClip& clip, double ticks, SkinnedMeshRenderer* smr);

/// @note 前回時刻は含めず、現在時刻までに通過したイベントをクリップの格納順で通知する。
void DispatchAnimationEvents(GameObject& owner, AnimatorComponent& animator, const asset::AnimationClip& clip,
    float previousTime, float currentTime, bool looped, bool reverse);

}
}
