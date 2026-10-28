/// @file    BonePoseHelpers.cpp
/// @brief   骨姿勢の共通探索とスキニング行列・Transform の同期。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include "BonePoseHelpers.hpp"

#include <Engine/Asset/Skeleton.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Math/Matrix4.hpp>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace fbzz::scene::bone_pose_detail {

namespace {

void RecalcBoneMatrix(const asset::Skeleton& skeleton,
                      const std::vector<math::Matrix4>& nodeGlobalTransforms,
                      std::vector<math::Matrix4>& boneMatrices,
                      int nodeIndex)
{
    if (nodeIndex < 0 ||
        nodeIndex >= static_cast<int>(skeleton.nodes.size()) ||
        nodeIndex >= static_cast<int>(nodeGlobalTransforms.size()))
        return;

    const int boneIndex = skeleton.nodes[static_cast<std::size_t>(nodeIndex)].boneIndex;
    if (boneIndex < 0 ||
        boneIndex >= static_cast<int>(boneMatrices.size()) ||
        boneIndex >= static_cast<int>(skeleton.bones.size()))
        return;

    boneMatrices[static_cast<std::size_t>(boneIndex)] =
        skeleton.rootInverseTransform
      * nodeGlobalTransforms[static_cast<std::size_t>(nodeIndex)]
      * skeleton.bones[static_cast<std::size_t>(boneIndex)].offsetMatrix;
}

}

SkinnedMeshRenderer* FindSkinnedMeshRenderer(GameObject& owner)
{
    if (auto* renderer = owner.GetComponent<SkinnedMeshRenderer>()) return renderer;
    for (int i = 0, count = owner.GetChildCount(); i < count; ++i) {
        if (auto* child = owner.GetChild(i)) {
            if (auto* renderer = child->GetComponent<SkinnedMeshRenderer>()) return renderer;
        }
    }
    return nullptr;
}

std::string CanonicalBoneName(std::string name)
{
    std::replace(name.begin(), name.end(), '\\', '/');
    const std::string helper = "_$AssimpFbx$_";
    if (const std::size_t helperPos = name.find(helper); helperPos != std::string::npos)
        name = name.substr(0, helperPos);
    if (const std::size_t pathPos = name.find_last_of('/'); pathPos != std::string::npos)
        name = name.substr(pathPos + 1);
    return name;
}

GameObject* BoneObject(Scene& scene, const SkinnedMeshRenderer& renderer, int nodeIndex)
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(renderer.nodeEntities.size()))
        return nullptr;
    return scene.GetGameObject(renderer.nodeEntities[static_cast<std::size_t>(nodeIndex)]);
}

void CommitBoneWorldPose(Scene& scene,
                         const asset::Skeleton& skeleton,
                         const SkinnedMeshRenderer& renderer,
                         AnimatorComponent& animator,
                         const math::Matrix4& ownerInv,
                         int nodeIndex,
                         const math::Vector3& worldPosition,
                         const math::Quaternion& worldRotation,
                         const math::Vector3& worldScale)
{
    if (nodeIndex < 0 ||
        nodeIndex >= static_cast<int>(renderer.nodeEntities.size()) ||
        nodeIndex >= static_cast<int>(animator.nodeGlobalTransforms.size()))
        return;

    const math::Matrix4 rootTransform =
        math::Matrix4::Inverse(skeleton.rootInverseTransform);
    animator.nodeGlobalTransforms[static_cast<std::size_t>(nodeIndex)] =
        rootTransform * ownerInv *
        math::Matrix4::TRS(worldPosition, worldRotation, worldScale);
    RecalcBoneMatrix(skeleton, animator.nodeGlobalTransforms, animator.boneMatrices, nodeIndex);

    if (GameObject* bone = BoneObject(scene, renderer, nodeIndex))
        SetWorldPose(*bone, worldPosition, worldRotation, worldScale);
}

}
