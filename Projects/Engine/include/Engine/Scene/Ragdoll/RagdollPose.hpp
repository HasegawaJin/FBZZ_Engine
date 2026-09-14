/// @file    RagdollPose.hpp
/// @brief   物理姿勢の階層ブレンドと、物理を持たない子孫への姿勢伝播。
/// @author  Hasegawa Jin
/// @date    2026-09-13
#pragma once
#include <Engine/Asset/Skeleton.hpp>
#include <Engine/Scene/Ragdoll/RagdollRig.hpp>

namespace fbzz::scene {

/// 親が先の配列。立位では親子間の並進を FK から保ち、ブレンド途中の骨長も変えない。
inline void BlendRagdollPose(const std::vector<RagdollBonePose>& targets, float weight,
                             bool preserveLengths, std::vector<math::Vector3>& positions,
                             std::vector<math::Quaternion>& rotations)
{
    for (std::size_t i = 0; i < targets.size(); ++i) {
        rotations[i] = math::Quaternion::Slerp(targets[i].rotation, rotations[i], weight);
        const int parent = targets[i].parent;
        if (preserveLengths && parent >= 0 && parent < static_cast<int>(i)) {
            const auto p = static_cast<std::size_t>(parent);
            positions[i] = positions[p] + rotations[p] * (targets[p].rotation.Inverse() *
                (targets[i].position - targets[p].position));
        } else {
            positions[i] = math::Vector3::Lerp(targets[i].position, positions[i], weight);
        }
    }
}

/// selected のノードは物理で確定済み。それ以外は元の親ローカル行列を最終姿勢へ掛ける。
inline void PropagateRagdollDescendants(const asset::Skeleton& skeleton,
                                        const std::vector<math::Matrix4>& original,
                                        const std::vector<bool>& selected,
                                        std::vector<math::Matrix4>& globals)
{
    if (original.size() != skeleton.nodes.size() || globals.size() != original.size() ||
        selected.size() != original.size()) return;
    std::vector<int> pending;
    for (std::size_t i = 0; i < skeleton.nodes.size(); ++i)
        if (skeleton.nodes[i].parentIndex < 0) pending.push_back(static_cast<int>(i));
    std::vector<bool> visited(skeleton.nodes.size(), false);
    while (!pending.empty()) {
        const int node = pending.back();
        pending.pop_back();
        if (node < 0 || node >= static_cast<int>(globals.size())) continue;
        const auto n = static_cast<std::size_t>(node);
        if (visited[n]) continue;
        visited[n] = true;
        const int parent = skeleton.nodes[n].parentIndex;
        if (!selected[n] && parent >= 0 && parent < static_cast<int>(globals.size())) {
            const auto p = static_cast<std::size_t>(parent);
            globals[n] = globals[p] * (math::Matrix4::Inverse(original[p]) * original[n]);
        }
        for (const int child : skeleton.nodes[n].children) pending.push_back(child);
    }
}

} // namespace fbzz::scene
