/// @file    BonePoseHelpers.hpp
/// @brief   骨姿勢システムが共有する探索とワールド姿勢の書き戻し契約。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once

#include <string>

namespace fbzz::asset {
struct Skeleton;
}

namespace fbzz::math {
struct Matrix4;
struct Quaternion;
struct Vector3;
}

namespace fbzz::scene {
struct AnimatorComponent;
class GameObject;
class Scene;
struct SkinnedMeshRenderer;
}

namespace fbzz::scene::bone_pose_detail {

/// @return オーナー、直下の子の順で最初の Renderer。孫以下は探索せず、未発見なら nullptr。
SkinnedMeshRenderer* FindSkinnedMeshRenderer(GameObject& owner);

/// @note Assimp FBX の補助接尾辞とパスだけを除去し、namespace と '|' は保持する。
std::string CanonicalBoneName(std::string name);

/// @return 範囲外・削除済みのノードなら nullptr。返す参照は Scene が所有する。
GameObject* BoneObject(Scene& scene, const SkinnedMeshRenderer& renderer, int nodeIndex);

/// @brief スキニング行列とボーン Transform に同じワールド姿勢を確定する。
/// @param ownerInv スケルトンを所有する GameObject のワールド行列の逆行列。
/// @pre 親のワールド姿勢は確定済みで、姿勢引数は書き戻す Transform のメンバーを参照しないこと。
/// @note ボーンの world = ownerWorld * rootInverse * nodeGlobal。逆変換でも rootInverse を戻す。
/// @note local も更新し、後続の SocketAttachment と FlushWorldTransforms に姿勢を維持する。
/// @note nodeEntities または nodeGlobalTransforms の範囲外なら変更しない。ボーン個体がなくても行列は更新する。
/// @see Docs/design/script-transform-contract.md
void CommitBoneWorldPose(Scene& scene,
                         const asset::Skeleton& skeleton,
                         const SkinnedMeshRenderer& renderer,
                         AnimatorComponent& animator,
                         const math::Matrix4& ownerInv,
                         int nodeIndex,
                         const math::Vector3& worldPosition,
                         const math::Quaternion& worldRotation,
                         const math::Vector3& worldScale);

}
