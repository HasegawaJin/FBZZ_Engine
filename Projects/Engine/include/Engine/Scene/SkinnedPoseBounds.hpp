/// @file    SkinnedPoseBounds.hpp
/// @brief   最終ボーン姿勢からカリング用のオーナーローカル境界を求める。
/// @author  Hasegawa Jin
/// @date    2026-09-13
#pragma once
#include <Engine/Asset/Skeleton.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <algorithm>

namespace fbzz::scene {

inline void UpdateSkinnedPoseBounds(AnimatorComponent& animator, const asset::Skeleton& skeleton)
{
    math::Vector3 lo{ 1.0e18f, 1.0e18f, 1.0e18f };
    math::Vector3 hi{ -1.0e18f, -1.0e18f, -1.0e18f };
    bool any = false;
    for (const auto& bone : skeleton.bones) {
        if (bone.nodeIndex < 0 || bone.nodeIndex >= static_cast<int>(animator.nodeGlobalTransforms.size()))
            continue;
        const auto matrix = skeleton.rootInverseTransform *
            animator.nodeGlobalTransforms[static_cast<std::size_t>(bone.nodeIndex)];
        const math::Vector3 p{ matrix.m[0][3], matrix.m[1][3], matrix.m[2][3] };
        lo = { std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z) };
        hi = { std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z) };
        any = true;
    }
    animator.skinnedBoundsCenter = any ? (lo + hi) * 0.5f : math::Vector3::ZERO;
    animator.skinnedBoundsRadius = any ? (hi - lo).Length() * 0.5f : 0.0f;
}

} // namespace fbzz::scene
