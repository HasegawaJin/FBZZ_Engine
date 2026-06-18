// FBZZ Engine
// FootIKSystem.cpp | fbzz::scene
// Humanoid 向けに FK の足運びを保ったまま足元のめり込みだけを補正する。
// WHY: 汎用 2-Bone IK はターゲット・ポール・膝制御まで扱うため、完成済み歩行クリップに常時適用すると
//      左右前後の足運びを固定しやすい。FootIK は接地専用パスとして Y 補正に責務を限定する。
#include <Engine/Scene/Systems/FootIKSystem.hpp>
#include "Engine/Core/Scheduler/SystemContext.hpp"
#include "Engine/Scene/Systems/AnimatorSystem.hpp"
#include "Engine/Scene/Systems/IKSystem.hpp"
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/BoneComponent.hpp>
#include <Engine/Scene/Components/FootIKComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Asset/Skeleton.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Physics/World.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace fbzz::scene {

namespace {

struct SkinningCB {
    math::Matrix4 boneMatrices[asset::MAX_SKINNING_BONES];
};

struct HumanoidLegNames {
    const char* root = "";
    const char* mid = "";
    const char* foot = "";
    const char* toe = "";
};

using GroundHit = physics::World::RaycastHit;

const physics::World::ColliderFilter kGroundFilter = [](const physics::ColliderInstance& inst) {
    return !inst.isTrigger && (!inst.body || inst.body->IsStatic());
};

math::Vector3 ArbitraryPerpendicular(const math::Vector3& axis)
{
    math::Vector3 perp = math::Vector3::Cross(math::Vector3::RIGHT, axis);
    if (perp.LengthSq() < math::EPSILON * math::EPSILON)
        perp = math::Vector3::Cross(math::Vector3::UP, axis);
    return perp.Normalized();
}

math::Quaternion FromToRotation(const math::Vector3& from, const math::Vector3& to)
{
    const math::Vector3 f = from.Normalized();
    const math::Vector3 t = to.Normalized();
    const float d = math::Vector3::Dot(f, t);

    if (d >= 1.0f - math::EPSILON)
        return math::Quaternion::Identity();

    if (d <= -1.0f + math::EPSILON)
        return math::Quaternion::FromAxisAngle(ArbitraryPerpendicular(f), math::PI);

    const math::Vector3 axis = math::Vector3::Cross(f, t);
    return math::Quaternion{ axis.x, axis.y, axis.z, 1.0f + d }.Normalized();
}

bool QueryGroundHit(physics::World& world,
                    const math::Vector3& footFkPosition,
                    float legLength,
                    const FootIKComponent& footIK,
                    GroundHit& outHit)
{
    const float rayStartHeight = legLength * footIK.rayUpRatio;
    const float rayDistance    = legLength * footIK.rayDownRatio;
    const math::Vector3 rayOrigin = footFkPosition + math::Vector3::UP * rayStartHeight;
    return world.Raycast(rayOrigin, -math::Vector3::UP, rayDistance, outHit, kGroundFilter);
}

void RecalcBoneMatrix(const asset::Skeleton& skeleton,
                      const std::vector<math::Matrix4>& nodeGlobalTransforms,
                      std::vector<math::Matrix4>& boneMatrices,
                      int nodeIndex)
{
    const int boneIndex = skeleton.nodes[static_cast<size_t>(nodeIndex)].boneIndex;
    if (boneIndex < 0 ||
        boneIndex >= static_cast<int>(boneMatrices.size()) ||
        boneIndex >= static_cast<int>(skeleton.bones.size()))
        return;

    const auto& bone = skeleton.bones[static_cast<size_t>(boneIndex)];
    boneMatrices[static_cast<size_t>(boneIndex)] =
        skeleton.rootInverseTransform
      * nodeGlobalTransforms[static_cast<size_t>(nodeIndex)]
      * bone.offsetMatrix;
}

void UploadBoneMatrices(AnimatorComponent& animator, renderer::ResourceManager& resources)
{
    SkinningCB cb{};
    for (int i = 0; i < asset::MAX_SKINNING_BONES; ++i)
        cb.boneMatrices[i] = math::Matrix4::Identity();

    for (size_t i = 0; i < animator.boneMatrices.size(); ++i)
        cb.boneMatrices[i] = animator.boneMatrices[i];

    resources.Update(animator.skinningBuffer, &cb, sizeof(SkinningCB));
}

SkinnedMeshRenderer* FindSkinnedMeshRenderer(GameObject& owner)
{
    if (auto* smr = owner.GetComponent<SkinnedMeshRenderer>())
        return smr;

    for (int i = 0, n = owner.GetChildCount(); i < n; ++i) {
        if (auto* child = owner.GetChild(i)) {
            if (auto* smr = child->GetComponent<SkinnedMeshRenderer>())
                return smr;
        }
    }
    return nullptr;
}

void ApplyNodeWorldPose(const asset::Skeleton& skeleton,
                        const SkinnedMeshRenderer& smr,
                        Scene& scene,
                        AnimatorComponent& animator,
                        const math::Matrix4& ownerInv,
                        int nodeIndex,
                        const math::Vector3& worldDelta,
                        const math::Quaternion* overrideRotation)
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(skeleton.nodes.size()))
        return;
    if (nodeIndex >= static_cast<int>(smr.nodeEntities.size()))
        return;
    if (nodeIndex >= static_cast<int>(animator.nodeGlobalTransforms.size()))
        return;

    const GameObject* boneGo = scene.GetGameObject(smr.nodeEntities[static_cast<size_t>(nodeIndex)]);
    if (!boneGo)
        return;

    const auto& tf = boneGo->transform;
    const math::Quaternion rotation = overrideRotation ? *overrideRotation : tf.worldRotation;
    animator.nodeGlobalTransforms[static_cast<size_t>(nodeIndex)] =
        ownerInv * math::Matrix4::TRS(
            tf.worldPosition + worldDelta,
            rotation,
            tf.worldScale);
    RecalcBoneMatrix(skeleton, animator.nodeGlobalTransforms, animator.boneMatrices, nodeIndex);
}

void ApplyNodeAndDescendantsOffset(const asset::Skeleton& skeleton,
                                   const SkinnedMeshRenderer& smr,
                                   Scene& scene,
                                   AnimatorComponent& animator,
                                   const math::Matrix4& ownerInv,
                                   int nodeIndex,
                                   const math::Vector3& worldDelta,
                                   const math::Quaternion* rootRotation = nullptr)
{
    ApplyNodeWorldPose(skeleton, smr, scene, animator, ownerInv, nodeIndex, worldDelta, rootRotation);

    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(skeleton.nodes.size()))
        return;

    for (int childIndex : skeleton.nodes[static_cast<size_t>(nodeIndex)].children) {
        ApplyNodeAndDescendantsOffset(
            skeleton, smr, scene, animator, ownerInv, childIndex, worldDelta, nullptr);
    }
}

std::string CanonicalBoneName(std::string name)
{
    std::replace(name.begin(), name.end(), '\\', '/');

    const std::string helper = "_$AssimpFbx$_";
    const size_t helperPos = name.find(helper);
    if (helperPos != std::string::npos)
        name = name.substr(0, helperPos);

    const size_t pathPos = name.find_last_of("/|");
    if (pathPos != std::string::npos)
        name = name.substr(pathPos + 1);

    const size_t namespacePos = name.find_last_of(':');
    if (namespacePos != std::string::npos)
        name = name.substr(namespacePos + 1);

    return name;
}

int FindHumanoidNode(const asset::Skeleton& skeleton, const char* humanoidName)
{
    if (!humanoidName || humanoidName[0] == '\0')
        return -1;

    const auto exact = skeleton.nodeMap.find(humanoidName);
    if (exact != skeleton.nodeMap.end())
        return exact->second;

    for (size_t i = 0; i < skeleton.nodes.size(); ++i) {
        if (CanonicalBoneName(skeleton.nodes[i].name) == humanoidName)
            return static_cast<int>(i);
    }
    return -1;
}

bool ResolveLegNodes(const asset::Skeleton& skeleton,
                     const HumanoidLegNames& leg,
                     int& outRoot,
                     int& outMid,
                     int& outFoot)
{
    outRoot = FindHumanoidNode(skeleton, leg.root);
    outMid  = FindHumanoidNode(skeleton, leg.mid);
    outFoot = FindHumanoidNode(skeleton, leg.foot);
    if (outRoot < 0 || outMid < 0 || outFoot < 0)
        return false;
    return true;
}

bool IsDescendantNode(const asset::Skeleton& skeleton, int ancestorNode, int node)
{
    if (ancestorNode < 0 || ancestorNode >= static_cast<int>(skeleton.nodes.size()))
        return false;

    for (int childNode : skeleton.nodes[static_cast<size_t>(ancestorNode)].children) {
        if (childNode == node)
            return true;
        if (IsDescendantNode(skeleton, childNode, node))
            return true;
    }
    return false;
}

bool ApplyFootCorrection(Scene& scene,
                         physics::World& world,
                         const asset::Skeleton& skeleton,
                         const SkinnedMeshRenderer& smr,
                         AnimatorComponent& animator,
                         const math::Matrix4& ownerInv,
                         const FootIKComponent& footIK,
                         const HumanoidLegNames& leg,
                         float effectiveWeight)
{
    int rootNode = -1;
    int midNode = -1;
    int footNode = -1;
    if (!ResolveLegNodes(skeleton, leg, rootNode, midNode, footNode))
        return false;

    const size_t nRoot = static_cast<size_t>(rootNode);
    const size_t nMid  = static_cast<size_t>(midNode);
    const size_t nFoot = static_cast<size_t>(footNode);
    if (nRoot >= smr.nodeEntities.size() ||
        nMid  >= smr.nodeEntities.size() ||
        nFoot >= smr.nodeEntities.size() ||
        nRoot >= animator.nodeGlobalTransforms.size() ||
        nMid  >= animator.nodeGlobalTransforms.size() ||
        nFoot >= animator.nodeGlobalTransforms.size())
        return false;

    const GameObject* rootGo = scene.GetGameObject(smr.nodeEntities[nRoot]);
    const GameObject* midGo  = scene.GetGameObject(smr.nodeEntities[nMid]);
    const GameObject* footGo = scene.GetGameObject(smr.nodeEntities[nFoot]);
    if (!rootGo || !midGo || !footGo)
        return false;

    const float upperLen = (midGo->transform.worldPosition - rootGo->transform.worldPosition).Length();
    const float lowerLen = (footGo->transform.worldPosition - midGo->transform.worldPosition).Length();
    const float legLength = upperLen + lowerLen;
    if (legLength <= math::EPSILON)
        return false;

    GroundHit hit;
    if (!QueryGroundHit(world, footGo->transform.worldPosition, legLength, footIK, hit))
        return false;

    const float desiredY = hit.point.y + footIK.footSurfaceOffset;
    const float rawCorrectionY = desiredY - footGo->transform.worldPosition.y;
    if (rawCorrectionY <= footIK.correctionDeadZone)
        return false;

    const float correctionY =
        math::Clamp(rawCorrectionY - footIK.correctionDeadZone, 0.0f, footIK.maxCorrection) *
        effectiveWeight;
    if (correctionY <= math::EPSILON)
        return false;

    math::Quaternion footRotation = footGo->transform.worldRotation;
    const float normalAxisLenSq = footIK.footNormalAxis.LengthSq();
    if (normalAxisLenSq > math::EPSILON * math::EPSILON) {
        const math::Vector3 footFloorDir =
            (footGo->transform.worldRotation * footIK.footNormalAxis.Normalized()).Normalized();
        const math::Quaternion groundRot =
            (FromToRotation(footFloorDir, hit.normal) * footGo->transform.worldRotation).Normalized();
        footRotation = math::Quaternion::Slerp(footGo->transform.worldRotation, groundRot, effectiveWeight);
    }

    ApplyNodeAndDescendantsOffset(
        skeleton,
        smr,
        scene,
        animator,
        ownerInv,
        footNode,
        math::Vector3{ 0.0f, correctionY, 0.0f },
        &footRotation);
    const int toeNode = FindHumanoidNode(skeleton, leg.toe);
    if (toeNode >= 0) {
        if (toeNode != footNode && !IsDescendantNode(skeleton, footNode, toeNode)) {
            ApplyNodeAndDescendantsOffset(
                skeleton,
                smr,
                scene,
                animator,
                ownerInv,
                toeNode,
                math::Vector3{ 0.0f, correctionY, 0.0f });
        }
    }
    return true;
}

} // namespace

ComponentAccess FootIKSystem::GetAccess() const
{
    return ComponentAccess{}
        .Reads<FootIKComponent>()
        .Writes<AnimatorComponent, BoneComponent>();
}

OrderingHints FootIKSystem::GetOrder() const
{
    return OrderingHints{}.After<AnimatorSystem>().Before<IKSystem>();
}

void FootIKSystem::Update(SystemContext& ctx)
{
    if (!ctx.resources) return;

    Scene& scene = ctx.scene;
    physics::World& world = ctx.world;
    renderer::ResourceManager& resources = *ctx.resources;
    const auto span = scene.GetEntities<FootIKComponent>();
    const auto entities = std::vector<EntityID>(span.begin(), span.end());

    for (EntityID id : entities) {
        GameObject* go = scene.GetGameObject(id);
        if (!go) continue;

        auto* footIK = go->GetComponent<FootIKComponent>();
        auto* animator = go->GetComponent<AnimatorComponent>();
        auto* smr = FindSkinnedMeshRenderer(*go);
        if (!footIK || !footIK->enabled || !animator || !smr) continue;
        if (!smr->model || !smr->model->skeleton) continue;
        if (animator->nodeGlobalTransforms.empty() || animator->boneMatrices.empty()) continue;
        if (smr->nodeEntities.empty()) continue;

        const float stateWeight = footIK->useAnimatorIKWeight ? animator->GetCurrentIKWeight() : 1.0f;
        const float effectiveWeight = math::Clamp01(footIK->weight * stateWeight);
        if (effectiveWeight <= math::EPSILON) continue;

        const asset::Skeleton& skeleton = *smr->model->skeleton;
        if (skeleton.nodes.empty() || skeleton.bones.empty()) continue;
        if (animator->nodeGlobalTransforms.size() < skeleton.nodes.size()) continue;
        const math::Matrix4 ownerInv = math::Matrix4::Inverse(go->transform.GetWorldMatrix());
        bool modified = false;

        const HumanoidLegNames leftLeg{ "LeftUpLeg", "LeftLeg", "LeftFoot", "LeftToeBase" };
        const HumanoidLegNames rightLeg{ "RightUpLeg", "RightLeg", "RightFoot", "RightToeBase" };

        modified |= ApplyFootCorrection(
            scene, world, skeleton, *smr, *animator, ownerInv, *footIK, leftLeg, effectiveWeight);
        modified |= ApplyFootCorrection(
            scene, world, skeleton, *smr, *animator, ownerInv, *footIK, rightLeg, effectiveWeight);

        if (modified && animator->skinningBuffer.IsValid())
            UploadBoneMatrices(*animator, resources);
    }
}

} // namespace fbzz::scene
