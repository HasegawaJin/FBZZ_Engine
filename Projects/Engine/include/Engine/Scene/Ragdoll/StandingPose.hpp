/// @file    StandingPose.hpp
/// @brief   骨長を保ったまま階層全体を立位の許容範囲へ射影する。
/// @author  Hasegawa Jin
/// @date    2026-09-12
#pragma once
#include <Engine/Scene/Ragdoll/RagdollRig.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::scene {

/// 親は子より前。根位置と親子間のローカル並進を保持し、回転差を階層全体で縮める。
/// 非有限な物理解は目標へ戻して false を返す。目標は有効な正規化済み FK 姿勢であること。
inline bool LimitStandingPose(const std::vector<RagdollBonePose>& targets,
                              float maxDistance, float maxRadians,
                              std::vector<math::Vector3>& positions,
                              std::vector<math::Quaternion>& rotations,
                              std::vector<math::Quaternion>& localRotations)
{
    const auto restore = [&] {
        positions.resize(targets.size());
        rotations.resize(targets.size());
        for (std::size_t i = 0; i < targets.size(); ++i) {
            positions[i] = targets[i].position;
            rotations[i] = targets[i].rotation;
        }
    };
    if (positions.size() != targets.size() || rotations.size() != targets.size()) {
        restore();
        return false;
    }
    for (std::size_t i = 0; i < targets.size(); ++i) {
        const float norm = math::Quaternion::Dot(rotations[i], rotations[i]);
        if (!std::isfinite(positions[i].LengthSq()) || !std::isfinite(norm) || norm < 1.0e-6f) {
            restore();
            return false;
        }
    }

    // ワールド位置を個別に丸めると骨が伸びる。候補のローカル回転だけを補間する。
    localRotations.resize(rotations.size());
    for (std::size_t i = 0; i < targets.size(); ++i) {
        const int parent = targets[i].parent;
        localRotations[i] = parent >= 0 && parent < static_cast<int>(i)
            ? (rotations[static_cast<std::size_t>(parent)].Inverse() * rotations[i]).Normalized()
            : rotations[i].Normalized();
    }
    const float distanceLimit = std::max(maxDistance, 0.0f);
    const float cosineLimit = std::cos(std::clamp(maxRadians, 0.0f, 3.14159265f) * 0.5f);
    const auto compose = [&](float weight) {
        bool inside = true;
        for (std::size_t i = 0; i < targets.size(); ++i) {
            const auto& target = targets[i];
            if (target.parent >= 0 && target.parent < static_cast<int>(i)) {
                const auto p = static_cast<std::size_t>(target.parent);
                const auto inverseParent = targets[p].rotation.Inverse();
                const auto localTarget = (inverseParent * target.rotation).Normalized();
                rotations[i] = (rotations[p] * math::Quaternion::Slerp(
                    localTarget, localRotations[i], weight)).Normalized();
                positions[i] = positions[p] + rotations[p] *
                    (inverseParent * (target.position - targets[p].position));
            } else {
                positions[i] = target.position;
                rotations[i] = math::Quaternion::Slerp(target.rotation, localRotations[i], weight);
            }
            inside = inside && (positions[i] - target.position).Length() <= distanceLimit &&
                std::abs(math::Quaternion::Dot(rotations[i], target.rotation)) >= cosineLimit;
        }
        return inside;
    };
    if (compose(1.0f)) return true;
    // 1 → 0.5 → 0.25 の段階的な制限は境界で反動を跳ね返す。許容側の境界まで連続的に戻す。
    float lower = 0.0f;
    float upper = 1.0f;
    for (int attempt = 0; attempt < 16; ++attempt) {
        const float middle = (lower + upper) * 0.5f;
        if (compose(middle)) lower = middle;
        else upper = middle;
    }
    if (lower <= 0.0f || !compose(lower)) restore();
    return true;
}

} // namespace fbzz::scene
