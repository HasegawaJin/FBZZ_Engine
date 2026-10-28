/// @file    AnimationClipHelpers.cpp
/// @brief   同 Source の先頭へフォールバックするロード済みクリップの探索。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include "AnimationClipHelpers.hpp"
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <cstddef>

namespace fbzz::scene::animation_clip_detail {

const asset::AnimationClip* FindClipBySource(const AnimatorComponent& animator,
                                             const std::string& sourcePath,
                                             const std::string& clipName)
{
    if (sourcePath.empty()) return nullptr;
    const asset::AnimationClip* firstFromSource = nullptr;
    for (size_t i = 0; i < animator.ClipCount(); ++i) {
        if (i >= animator.clipSourcePaths.size() ||
            animator.clipSourcePaths[i] != sourcePath) continue;
        if (!firstFromSource) firstFromSource = &animator.ClipAt(i);
        if (!clipName.empty() && animator.ClipAt(i).name == clipName)
            return &animator.ClipAt(i);
    }
    return firstFromSource;
}

}
