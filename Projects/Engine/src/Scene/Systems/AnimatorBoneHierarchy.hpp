/// @file    AnimatorBoneHierarchy.hpp
/// @brief   Animator がシーンのボーンとスキニング行列を更新する内部境界。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once
#include <cstdint>
#include <vector>

namespace fbzz::asset {
struct Skeleton;
}

namespace fbzz::math {
struct Matrix4;
}

namespace fbzz::scene {
class GameObject;
class Scene;
struct AnimatorComponent;
struct SkinnedMeshRenderer;
struct Transform;

namespace animator_detail {

void UpdateWorldTransform(GameObject& go, const Transform& parentTransform);
void PropagateNonBoneChildTransforms(GameObject& parent);

/// @note スキニング空間に変換済みのルート local TRS を、レイヤーの評価前にアニメーション空間へ戻す。
void SetRootBoneLocalInAnimationSpace(GameObject& rootBone, const math::Matrix4& rootInverseTransform);

/// @return 不足ノードを全て生成する容量がない場合は、既存階層と Renderer の束縛を維持して false。
bool EnsureBoneHierarchy(Scene& scene, GameObject& owner, SkinnedMeshRenderer& smr, const asset::Skeleton& skeleton);

/// @note 停止中もボーンとアタッチメントの位置を、描画されるバインド姿勢に合わせる。
void ApplyBindPoseToBones(Scene& scene, const asset::Skeleton& skeleton, SkinnedMeshRenderer& smr, GameObject& owner);

/// @pre visited は skeleton.nodes と同じ長さで全要素を 0 に初期化すること。
void PublishSkeletonPose(Scene& scene, GameObject& owner, const asset::Skeleton& skeleton,
    SkinnedMeshRenderer& smr, AnimatorComponent& animator, std::vector<uint8_t>& visited);

void PublishSkeletonPose(Scene& scene, GameObject& owner, const asset::Skeleton& skeleton,
    SkinnedMeshRenderer& smr, AnimatorComponent& animator);

}
}
