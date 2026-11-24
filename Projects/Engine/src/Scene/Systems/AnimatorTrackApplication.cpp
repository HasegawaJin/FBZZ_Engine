/// @file    AnimatorTrackApplication.cpp
/// @brief   クリップの非スケルタルトラックとイベントをシーンへ反映する。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include "AnimatorTrackApplication.hpp"

#include <Engine/Asset/AnimationClip.hpp>
#include <Engine/Asset/AnimationSampling.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/AnimationPropertyBinding.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/BoneComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/ScriptComponent.hpp>

namespace fbzz::scene::animator_detail {

/// @brief 階層パス ("Armature/Hips/Spine/Head") から対象 GameObject を引く。
/// @note 毎フレーム × Animator 数 × トラック数で呼ばれるため、区間は string_view で切りヒープ確保を避ける。
/// @return 見つからなければ nullptr。
GameObject* FindAnimationTarget(GameObject& root, const std::string_view pathView)
{
    if (pathView.empty() || pathView == "." || pathView == std::string_view{ root.name })
        return &root;

    GameObject* current = &root;
    size_t begin = 0;
    while (begin < pathView.size()) {
        const size_t end = pathView.find('/', begin);
        const std::string_view part = pathView.substr(
            begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
        begin = end == std::string_view::npos ? pathView.size() : end + 1;
        if (part.empty() || (current == &root && part == std::string_view{ root.name })) continue;
        GameObject* next = nullptr;
        for (int i = 0; i < current->GetChildCount(); ++i) {
            GameObject* child = current->GetChild(i);
            if (child && std::string_view{ child->name } == part) {
                next = child;
                break;
            }
        }
        if (!next) return nullptr;
        current = next;
    }
    return current;
}

namespace {

/// @brief targetPath が無い旧 .anim の対象を、BoneComponent の名前から解決する。
/// @note BoneComponent に限定し、同名の装飾用 GameObject を誤って動かさない。
GameObject* FindAnimationBoneByNameRecursive(GameObject& current,
                                             std::string_view canonicalName)
{
    if (auto* bone = current.GetComponent<BoneComponent>()) {
        if (asset::CanonicalNodeName(bone->boneName) == canonicalName ||
            asset::CanonicalNodeName(current.name) == canonicalName)
            return &current;
    }

    for (int i = 0; i < current.GetChildCount(); ++i) {
        GameObject* child = current.GetChild(i);
        if (!child) continue;
        if (GameObject* found = FindAnimationBoneByNameRecursive(*child, canonicalName))
            return found;
    }
    return nullptr;
}

/// @brief マテリアルアニメーションの適用先 (SkinnedMeshRenderer と MaterialComponent を持つ GO) を探す。
/// @note submesh は MaterialComponent のスロットなので、どのスロットへ書くかは ApplyMaterialProperty が決める。
GameObject* FindMaterialSlotTarget(GameObject& root)
{
    if (root.GetComponent<SkinnedMeshRenderer>() && root.GetComponent<MaterialComponent>())
        return &root;
    for (int i = 0; i < root.GetChildCount(); ++i) {
        GameObject* child = root.GetChild(i);
        if (!child) continue;
        if (GameObject* found = FindMaterialSlotTarget(*child)) return found;
    }
    return nullptr;
}

}

GameObject* FindAnimationTargetByNodeName(GameObject& root, std::string_view nodeName)
{
    const std::string canonicalName = asset::CanonicalNodeName(nodeName);
    if (canonicalName.empty()) return nullptr;
    return FindAnimationBoneByNameRecursive(root, canonicalName);
}

void ApplyTransformTracks(GameObject& root,
                          const asset::AnimationClip& clip,
                          double ticks)
{
    for (const auto& track : clip.tracks) {
        GameObject* target = track.targetPath.empty()
            ? nullptr : FindAnimationTarget(root, track.targetPath);
        if (!target) continue;
        target->transform.position = SampleVectorKeys(
            track.positions, ticks, target->transform.position, track.interp);
        target->transform.rotation = SampleQuaternionKeys(
            track.rotations, ticks, target->transform.rotation, track.interp);
        target->transform.scale = SampleVectorKeys(
            track.scales, ticks, target->transform.scale, track.interp);
    }
}

void ApplyPropertyTracks(GameObject& root,
                         const asset::AnimationClip& clip,
                         double ticks,
                         SkinnedMeshRenderer* smr)
{
    for (const auto& track : clip.propertyTracks) {
        if (track.targetType == asset::AnimTargetType::MorphWeight) {
            if (smr && !track.floatKeys.empty())
                smr->morphWeights[track.propertyName] =
                    SampleFloatKeys(track.floatKeys, ticks, track.interp);
            continue;
        }
        if (track.targetPath.empty()) continue;
        GameObject* target = FindAnimationTarget(root, track.targetPath);
        if (track.targetType == asset::AnimTargetType::MaterialProperty &&
            (!target || !target->GetComponent<MaterialComponent>()))
            target = FindMaterialSlotTarget(root);
        if (!target) continue;
        switch (track.targetType) {
        case asset::AnimTargetType::ComponentProperty:
            ApplyComponentProperty(*target, track, ticks);
            break;
        case asset::AnimTargetType::MaterialProperty:
            ApplyMaterialProperty(*target, track, ticks);
            break;
        case asset::AnimTargetType::MorphWeight:
            break;
        }
    }
}

void DispatchAnimationEvents(GameObject& owner,
                              AnimatorComponent& animator,
                              const asset::AnimationClip& clip,
                             float previousTime,
                             float currentTime,
                             bool looped,
                             bool reverse)
{
    if (clip.events.empty()) return;
    ScriptComponent* scripts = owner.GetComponent<ScriptComponent>();
    const float duration = static_cast<float>(clip.GetDurationSeconds());
    for (const auto& event : clip.events) {
        const float eventTime = static_cast<float>(event.time);
        bool crossed = false;
        if (!reverse)
            crossed = looped ? (eventTime > previousTime || eventTime <= currentTime)
                             : (eventTime > previousTime && eventTime <= currentTime);
        else
            crossed = looped ? (eventTime < previousTime || eventTime >= currentTime)
                             : (eventTime < previousTime && eventTime >= currentTime);
        if (!crossed || eventTime < 0.0f || eventTime > duration) continue;
        const AnimationEventInfo info{
            event.name.c_str(), event.intParam, event.floatParam, eventTime
        };
        animator.firedEvents.push_back({
            event.name, event.intParam, event.floatParam, eventTime, Time::frameCount });
        if (scripts != nullptr)
            for (auto& entry : scripts->scripts)
                if (entry.script)
                    entry.script->ExecuteProfiledCallback(&Script::OnAnimationEvent, info, ScriptCallbackKind::ANIMATION_EVENT, "OnAnimationEvent");
    }
}

}
