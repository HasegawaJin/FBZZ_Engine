/// @file    AnimatorBoneHierarchy.cpp
/// @brief   ボーン実体の束縛と階層・アタッチメントの更新、スキニング行列の再構築を担う。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include "AnimatorBoneHierarchy.hpp"
#include "AnimationWorkTrace.hpp"

#include <Engine/Asset/Model/Model.hpp>
#include <Engine/Core/Diagnostics/Logger.hpp>
#include <Engine/Scene/Components/Animation/AnimatorComponent.hpp>
#include <Engine/Scene/Components/Animation/BoneComponent.hpp>
#include <Engine/Scene/Components/Geometry/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Math/Scalar/MathUtils.hpp>
#include <algorithm>
#include <utility>

namespace fbzz::scene::animator_detail {

void UpdateWorldTransform(GameObject& go, const Transform& parentTransform)
{
    Transform& tf = go.transform;
    math::Vector3 scaledLocal = {
        tf.position.x * parentTransform.worldScale.x,
        tf.position.y * parentTransform.worldScale.y,
        tf.position.z * parentTransform.worldScale.z
    };
    tf.worldRotation   = (parentTransform.worldRotation * tf.rotation).Normalized();
    tf.worldPosition   = parentTransform.worldPosition + parentTransform.worldRotation * scaledLocal;
    tf.worldScale = {
        parentTransform.worldScale.x * tf.scale.x,
        parentTransform.worldScale.y * tf.scale.y,
        parentTransform.worldScale.z * tf.scale.z
    };
}

namespace {

/// @brief アフィン行列を TRS へ分解し、Transform の world* へ入れて返す。
/// @note せん断は捨てる。スケールが EPSILON 以下の軸は回転抽出で 0 扱いになる。
Transform DecomposeAffineMatrix(const math::Matrix4& worldMatrix)
{
    Transform result;
    result.worldPosition = {
        worldMatrix.m[0][3], worldMatrix.m[1][3], worldMatrix.m[2][3]
    };

    const math::Vector3 basisX{
        worldMatrix.m[0][0], worldMatrix.m[1][0], worldMatrix.m[2][0]
    };
    const math::Vector3 basisY{
        worldMatrix.m[0][1], worldMatrix.m[1][1], worldMatrix.m[2][1]
    };
    const math::Vector3 basisZ{
        worldMatrix.m[0][2], worldMatrix.m[1][2], worldMatrix.m[2][2]
    };
    result.worldScale = { basisX.Length(), basisY.Length(), basisZ.Length() };

    math::Matrix4 rotationMatrix = math::Matrix4::Identity();
    const float inverseScaleX = result.worldScale.x > math::EPSILON
        ? 1.0f / result.worldScale.x : 0.0f;
    const float inverseScaleY = result.worldScale.y > math::EPSILON
        ? 1.0f / result.worldScale.y : 0.0f;
    const float inverseScaleZ = result.worldScale.z > math::EPSILON
        ? 1.0f / result.worldScale.z : 0.0f;
    for (int row = 0; row < 3; ++row) {
        rotationMatrix.m[row][0] = worldMatrix.m[row][0] * inverseScaleX;
        rotationMatrix.m[row][1] = worldMatrix.m[row][1] * inverseScaleY;
        rotationMatrix.m[row][2] = worldMatrix.m[row][2] * inverseScaleZ;
    }
    result.worldRotation = math::Quaternion::FromMatrix4(rotationMatrix).Normalized();
    return result;
}

/// @note 後段の TransformSystem / ConstraintSystem が world を再計算するため、変換を local TRS に反映する。
void ApplyRootLocalTransform(GameObject& rootBone, const math::Matrix4& spaceTransform)
{
    const auto& transform = rootBone.transform;
    const auto local = spaceTransform * math::Matrix4::TRS(transform.position, transform.rotation, transform.scale);
    const Transform decomposed = DecomposeAffineMatrix(local);
    rootBone.transform.position = decomposed.worldPosition;
    rootBone.transform.rotation = decomposed.worldRotation;
    rootBone.transform.scale = decomposed.worldScale;
}

}

void SetRootBoneLocalInAnimationSpace(GameObject& rootBone, const math::Matrix4& rootInverseTransform)
{
    ApplyRootLocalTransform(rootBone, math::Matrix4::Inverse(rootInverseTransform));
}

/// @brief Bone 配下の非 Bone 子 (Particle / Attachment 等) の world 値を組み直す。
/// @note AnimatorSystem は TransformSystem の後に Bone を上書きするので、これが無いと子が追従しない。
/// @note BoneComponent を持つ子は Skeleton 再帰側が処理する。
void PropagateNonBoneChildTransforms(GameObject& parent)
{
    for (int i = 0; i < parent.GetChildCount(); ++i) {
        GameObject* child = parent.GetChild(i);
        if (!child) {
            continue;
        }
        if (child->GetComponent<BoneComponent>()) {
            continue;
        }

        UpdateWorldTransform(*child, parent.transform);
        PropagateNonBoneChildTransforms(*child);
    }
}

namespace {

GameObject* FindBoneDescendant(GameObject& root, int nodeIndex)
{
    for (int i = 0; i < root.GetChildCount(); ++i) {
        GameObject* child = root.GetChild(i);
        if (!child) {
            continue;
        }

        if (auto* bone = child->GetComponent<BoneComponent>())
            if (bone->nodeIndex == nodeIndex) {
                return child;
            }

        if (GameObject* found = FindBoneDescendant(*child, nodeIndex)) {
            return found;
        }
    }
    return nullptr;
}

/// @brief nodeIndex から根までの親参照が循環せず範囲内に収まるかを確かめる。
/// @note 古いキャッシュや破損 .fzasset は親インデックスが循環し、Bone 生成の再帰をハングさせる。
bool HasAcyclicParentChain(const asset::Skeleton& skeleton, int nodeIndex)
{
    std::vector<uint8_t> visited(skeleton.nodes.size(), 0);
    int current = nodeIndex;
    while (current >= 0) {
        if (current >= static_cast<int>(skeleton.nodes.size())) {
            return false;
        }
        if (visited[static_cast<size_t>(current)] != 0) {
            return false;
        }
        visited[static_cast<size_t>(current)] = 1;
        current = skeleton.nodes[static_cast<size_t>(current)].parentIndex;
    }
    return true;
}

/// @note 既存の起点ボーンが Renderer の兄弟にある場合も探す。別個体を束縛しないよう、その親までを探索範囲とする。
GameObject& BoneSearchScope(Scene& scene, GameObject& owner, const SkinnedMeshRenderer& smr)
{
    auto* root = scene.GetGameObject(smr.skeletonRootEntity);
    if (!root) {
        return owner;
    }
    auto* parent = root->GetParent();
    return parent ? *parent : *root;
}

/// @brief ノードに対応する Bone GameObject を既存から束縛し、無ければ親から順に生成する。
/// @return nodeIndex が範囲外なら owner、生成できなければ nullptr。
GameObject* EnsureBoneObject(Scene& scene,
                             GameObject& owner,
                             SkinnedMeshRenderer& smr,
                             const asset::Skeleton& skeleton,
                             int nodeIndex)
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(skeleton.nodes.size())) {
        return &owner;
    }
    auto& node = skeleton.nodes[static_cast<size_t>(nodeIndex)];

    if (nodeIndex < static_cast<int>(smr.nodeEntities.size())) {
        if (auto* existing = scene.GetGameObject(smr.nodeEntities[static_cast<size_t>(nodeIndex)])) {
            if (auto* bone = existing->GetComponent<BoneComponent>()) {
                bone->boneName = node.name;
                bone->boneIndex = node.boneIndex;
                bone->skinnedMeshEntity = owner.GetID();
            }
            if (nodeIndex == skeleton.rootNodeIndex) {
                smr.skeletonRootEntity = existing->GetID();
            }
            return existing;
        }
    }

    auto& searchScope = BoneSearchScope(scene, owner, smr);
    if (GameObject* found = FindBoneDescendant(searchScope, nodeIndex)) {
        if (auto* bone = found->GetComponent<BoneComponent>()) {
            bone->boneName = node.name;
            bone->boneIndex = node.boneIndex;
            bone->skinnedMeshEntity = owner.GetID();
        }
        smr.nodeEntities[static_cast<size_t>(nodeIndex)] = found->GetID();
        if (nodeIndex == skeleton.rootNodeIndex) {
            smr.skeletonRootEntity = found->GetID();
        }
        return found;
    }

    GameObject* parent = &owner;
    if (node.parentIndex >= 0 &&
        node.parentIndex < static_cast<int>(skeleton.nodes.size()) &&
        HasAcyclicParentChain(skeleton, node.parentIndex)) {
        parent = EnsureBoneObject(scene, owner, smr, skeleton, node.parentIndex);
    }
    if (!parent) {
        return nullptr;
    }

    GameObject* boneObject = scene.TryCreateGameObject(node.name);
    if (!boneObject) {
        return nullptr;
    }
    boneObject->layer = owner.layer;
    boneObject->transform.position = node.bindTranslation;
    boneObject->transform.rotation = node.bindRotation;
    boneObject->transform.scale = node.bindScale;
    boneObject->SetParent(parent);

    BoneComponent bone{};
    bone.boneName = node.name;
    bone.nodeIndex = nodeIndex;
    bone.boneIndex = node.boneIndex;
    bone.skinnedMeshEntity = owner.GetID();
    bone.generated = true;
    boneObject->AddComponent<BoneComponent>(std::move(bone));

    smr.nodeEntities[static_cast<size_t>(nodeIndex)] = boneObject->GetID();
    if (nodeIndex == skeleton.rootNodeIndex) {
        smr.skeletonRootEntity = boneObject->GetID();
    }
    return boneObject;
}

}

/// @return 不足ノードを全て作れる容量がなければ、既存の階層と Renderer の束縛を変えず false。
bool EnsureBoneHierarchy(Scene& scene,
                         GameObject& owner,
                         SkinnedMeshRenderer& smr,
                         const asset::Skeleton& skeleton)
{
    AnimationWorkScope hierarchyWork(AnimationWorkPart::BONE_HIERARCHY);
    if (smr.nodeEntities.size() == skeleton.nodes.size() &&
        std::all_of(smr.nodeEntities.begin(), smr.nodeEntities.end(),
                    [&scene](EntityID id) { return scene.GetGameObject(id) != nullptr; })) {
        for (size_t i = 0; i < skeleton.nodes.size(); ++i)
            if (!EnsureBoneObject(scene, owner, smr, skeleton, static_cast<int>(i))) {
                return false;
            }
        return true;
    }

    std::vector<EntityID> resolvedNodes(skeleton.nodes.size(), EntityID::INVALID);
    auto& searchScope = BoneSearchScope(scene, owner, smr);
    size_t missingCount = 0;
    for (size_t i = 0; i < skeleton.nodes.size(); ++i) {
        GameObject* existing = smr.nodeEntities.size() == skeleton.nodes.size()
            ? scene.GetGameObject(smr.nodeEntities[i]) : nullptr;
        if (!existing) {
            if (auto* bone = searchScope.GetComponent<BoneComponent>();
                bone && bone->nodeIndex == static_cast<int>(i)) {
                existing = &searchScope;
            }
            else {
                existing = FindBoneDescendant(searchScope, static_cast<int>(i));
            }
        }
        if (existing) {
            resolvedNodes[i] = existing->GetID();
        }
        else {
            ++missingCount;
        }
    }
    if (!scene.CanCreateGameObjects(missingCount)) {
        FBZZ_LOG_WARN("AnimatorSystem: '%s' needs %zu Bone entities, but only %zu slots remain",
                      owner.name.c_str(), missingCount, scene.RemainingEntityCapacity());
        return false;
    }
    smr.nodeEntities = std::move(resolvedNodes);

    for (size_t i = 0; i < skeleton.nodes.size(); ++i)
        if (!EnsureBoneObject(scene, owner, smr, skeleton, static_cast<int>(i))) {
            return false;
        }
    return true;
}

namespace {

/// @brief ボーン階層の world を積み始める親 Transform。root ボーンの実際の親で、無ければ owner。
/// @note owner 固定にすると owner と root ボーンの間に挟んだノード (体ごと傾ける等) の回転・移動が毎フレーム捨てられる。
/// @note スキニング行列は owner ローカル系で組むので、挟んだノードのぶんはメッシュにも当たり判定にも乗る。
const Transform& SkeletonParentTransform(Scene& scene,
                                         const SkinnedMeshRenderer& smr,
                                         const asset::Skeleton& skeleton,
                                         GameObject& owner)
{
    if (skeleton.rootNodeIndex < 0 ||
        skeleton.rootNodeIndex >= static_cast<int>(smr.nodeEntities.size())) {
        return owner.transform;
    }
    GameObject* rootBone =
        scene.GetGameObject(smr.nodeEntities[static_cast<size_t>(skeleton.rootNodeIndex)]);
    if (!rootBone) {
        return owner.transform;
    }
    GameObject* parent = rootBone->GetParent();
    if (!parent || parent == &owner) {
        return owner.transform;
    }
    return parent->transform;
}

/// @brief nodeIndex 以下のボーンと非ボーン子の world を親から順に組み直す。
/// @param[in,out] visited skeleton.nodes と同じ長さ。呼び出し側で 0 初期化する。
void PropagateBoneTransforms(Scene& scene,
                             const asset::Skeleton& skeleton,
                             SkinnedMeshRenderer& smr,
                             const math::Matrix4& rootInverseTransform,
                             int nodeIndex,
                             const Transform& parentTransform,
                             std::vector<uint8_t>& visited)
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(skeleton.nodes.size()) ||
        nodeIndex >= static_cast<int>(smr.nodeEntities.size())) {
        return;
    }
    // 破損アセットで children が循環していても無限再帰にしない。
    if (visited[static_cast<size_t>(nodeIndex)] != 0) {
        return;
    }
    visited[static_cast<size_t>(nodeIndex)] = 1;
    GameObject* boneObject = scene.GetGameObject(smr.nodeEntities[static_cast<size_t>(nodeIndex)]);
    if (!boneObject) {
        return;
    }

    if (nodeIndex == skeleton.rootNodeIndex) {
        // boneMatrix = rootInverse · nodeGlobal · offset の rootInverse を root local へ移し、Flush 後も残す。
        ApplyRootLocalTransform(*boneObject, rootInverseTransform);
    }

    UpdateWorldTransform(*boneObject, parentTransform);
    PropagateNonBoneChildTransforms(*boneObject);

    for (int child : skeleton.nodes[static_cast<size_t>(nodeIndex)].children) {
        PropagateBoneTransforms(scene, skeleton, smr, rootInverseTransform,
                                child, boneObject->transform, visited);
    }
}

void PropagateSkeletonWorld(Scene& scene, const asset::Skeleton& skeleton,
    SkinnedMeshRenderer& smr, GameObject& owner, std::vector<uint8_t>& visited)
{
    PropagateBoneTransforms(scene, skeleton, smr, skeleton.rootInverseTransform, skeleton.rootNodeIndex,
        SkeletonParentTransform(scene, smr, skeleton, owner), visited);
}

}

/// @brief ボーン GameObject をバインド姿勢へ戻し、world を組み直す。
/// @note 停止中も GPU は referencePose を描くので、ボーンに前フレーム姿勢を残すとソケットとメッシュがずれる。
void ApplyBindPoseToBones(Scene& scene,
                          const asset::Skeleton& skeleton,
                          SkinnedMeshRenderer& smr,
                          GameObject& owner)
{
    for (size_t i = 0; i < skeleton.nodes.size(); ++i) {
        if (i >= smr.nodeEntities.size()) {
            continue;
        }
        GameObject* boneObject = scene.GetGameObject(smr.nodeEntities[i]);
        if (!boneObject) {
            continue;
        }
        const auto& node = skeleton.nodes[i];
        boneObject->transform.position = node.bindTranslation;
        boneObject->transform.rotation = node.bindRotation;
        boneObject->transform.scale = node.bindScale;
    }

    std::vector<uint8_t> visited(skeleton.nodes.size(), 0);
    PropagateSkeletonWorld(scene, skeleton, smr, owner, visited);
}

namespace {

/// @brief ボーン GameObject の world から nodeGlobalTransforms と boneMatrices を組み直す。
/// @note 配列長が足りない (評価失敗・リグ差し替え直後) ときは何もしない。
void RebuildSkinningFromBoneTransforms(Scene& scene,
                                       GameObject& owner,
                                       const asset::Skeleton& skeleton,
                                       SkinnedMeshRenderer& smr,
                                       AnimatorComponent& animator)
{
    if (smr.nodeEntities.size() < skeleton.nodes.size() ||
        animator.nodeGlobalTransforms.size() < skeleton.nodes.size()) {
        return;
    }

    const math::Matrix4 ownerInverse = math::Matrix4::Inverse(owner.transform.GetWorldMatrix());
    // PropagateBoneTransforms が root local へ rootInverse を焼き込んでいるので、ここで打ち消す。
    const math::Matrix4 rootTransform =
        math::Matrix4::Inverse(skeleton.rootInverseTransform);

    for (size_t nodeIndex = 0; nodeIndex < skeleton.nodes.size(); ++nodeIndex) {
        GameObject* boneObject = scene.GetGameObject(smr.nodeEntities[nodeIndex]);
        if (!boneObject) {
            continue;
        }

        animator.nodeGlobalTransforms[nodeIndex] =
            rootTransform * ownerInverse * boneObject->transform.GetWorldMatrix();
    }

    for (size_t boneIndex = 0; boneIndex < animator.boneMatrices.size(); ++boneIndex) {
        if (boneIndex >= skeleton.bones.size()) {
            continue;
        }
        const auto& bone = skeleton.bones[boneIndex];
        if (bone.nodeIndex < 0 ||
            bone.nodeIndex >= static_cast<int>(animator.nodeGlobalTransforms.size())) {
            continue;
        }

        animator.boneMatrices[boneIndex] =
            skeleton.rootInverseTransform
          * animator.nodeGlobalTransforms[static_cast<size_t>(bone.nodeIndex)]
          * bone.offsetMatrix;
    }
}

}

void PublishSkeletonPose(Scene& scene, GameObject& owner, const asset::Skeleton& skeleton,
    SkinnedMeshRenderer& smr, AnimatorComponent& animator, std::vector<uint8_t>& visited)
{
    PropagateSkeletonWorld(scene, skeleton, smr, owner, visited);
    RebuildSkinningFromBoneTransforms(scene, owner, skeleton, smr, animator);
}

void PublishSkeletonPose(Scene& scene, GameObject& owner, const asset::Skeleton& skeleton,
    SkinnedMeshRenderer& smr, AnimatorComponent& animator)
{
    std::vector<uint8_t> visited(skeleton.nodes.size(), 0);
    PublishSkeletonPose(scene, owner, skeleton, smr, animator, visited);
}

}
