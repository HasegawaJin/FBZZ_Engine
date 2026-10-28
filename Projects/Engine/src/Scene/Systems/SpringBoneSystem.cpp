/// @file    SpringBoneSystem.cpp
/// @brief   揺れもの (二次モーション) の Verlet 積分と骨行列への書き戻し
/// @author  Hasegawa Jin
/// @date    2026-08-25
#include <Engine/Scene/Systems/SpringBoneSystem.hpp>
#include "BonePoseHelpers.hpp"

#include "Engine/Core/Scheduler/SystemContext.hpp"
#include "Engine/Scene/Systems/AnimatorSystem.hpp"
#include "Engine/Scene/Systems/IKSystem.hpp"
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/BoneComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/SpringBoneComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Asset/Skeleton.hpp>
#include <Engine/Profiler/ProfileScope.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace fbzz::scene {

namespace {

struct SkinningCB {
    math::Matrix4 boneMatrices[asset::MAX_SKINNING_BONES];
};

/// @note フレーム落ちで髪が伸び切るのを防ぐため刻みを固定し、余りは次フレームへ持ち越さず捨てる。
constexpr float kSubStep      = 1.0f / 60.0f;
constexpr int   kMaxSubSteps  = 4;

math::Vector3 ComponentScale(const math::Vector3& a, const math::Vector3& b)
{
    return { a.x * b.x, a.y * b.y, a.z * b.z };
}

math::Vector3 ArbitraryPerpendicular(const math::Vector3& axis)
{
    const math::Vector3 absoluteAxis{
        std::abs(axis.x), std::abs(axis.y), std::abs(axis.z)
    };
    math::Vector3 basis = math::Vector3::RIGHT;
    if (absoluteAxis.y <= absoluteAxis.x && absoluteAxis.y <= absoluteAxis.z)
        basis = math::Vector3::UP;
    else if (absoluteAxis.z <= absoluteAxis.x && absoluteAxis.z <= absoluteAxis.y)
        basis = math::Vector3::FORWARD;
    return math::Vector3::Cross(basis, axis).NormalizedOr(math::Vector3::UP);
}

math::Quaternion FromToRotation(const math::Vector3& from, const math::Vector3& to)
{
    const math::Vector3 f = from.NormalizedOr(math::Vector3::FORWARD);
    const math::Vector3 t = to.NormalizedOr(math::Vector3::FORWARD);
    const float d = math::Vector3::Dot(f, t);

    if (d >= 1.0f - math::EPSILON) return math::Quaternion::Identity();
    if (d <= -1.0f + math::EPSILON)
        return math::Quaternion::FromAxisAngle(ArbitraryPerpendicular(f), math::PI);

    const math::Vector3 axis = math::Vector3::Cross(f, t);
    return math::Quaternion{ axis.x, axis.y, axis.z, 1.0f + d }.Normalized();
}

int FindNode(const asset::Skeleton& skeleton, const std::string& boneName)
{
    if (boneName.empty()) return -1;
    if (const auto exact = skeleton.nodeMap.find(boneName); exact != skeleton.nodeMap.end())
        return exact->second;
    for (std::size_t i = 0; i < skeleton.nodes.size(); ++i) {
        if (bone_pose_detail::CanonicalBoneName(skeleton.nodes[i].name) == boneName)
            return static_cast<int>(i);
    }
    return -1;
}

void UploadBoneMatrices(AnimatorComponent& animator, renderer::ResourceManager& resources)
{
    SkinningCB cb{};
    for (int i = 0; i < asset::MAX_SKINNING_BONES; ++i)
        cb.boneMatrices[i] = math::Matrix4::Identity();
    for (std::size_t i = 0; i < animator.boneMatrices.size(); ++i)
        cb.boneMatrices[i] = animator.boneMatrices[i];
    resources.Update(animator.skinningBuffer, &cb, sizeof(SkinningCB));
}

/// @note 積分中に Transform を読み直さないため、このフレームの FK/IK ワールド姿勢を凍結する。
struct FkSnapshot {
    math::Vector3    position = math::Vector3::ZERO;
    math::Quaternion rotation = math::Quaternion::Identity();
    math::Vector3    scale    = math::Vector3::ONE;
    /// @note チェーン内の親から見たローカル姿勢。親の揺れ結果へ乗せ直すために使う。
    math::Vector3    localOffset   = math::Vector3::ZERO;
    math::Quaternion localRotation = math::Quaternion::Identity();
};

/// @note 位置と半径はワールド空間。
struct ResolvedCollider {
    math::Vector3 start  = math::Vector3::ZERO;
    math::Vector3 end    = math::Vector3::ZERO;
    float         radius = 0.0f;
    bool          isCapsule = false;
};

/// @note StepChain が確定した親姿勢を読むため、nodes は親が必ず先に来る順で組む。
void BuildChain(SpringBoneChain& chain,
                const asset::Skeleton& skeleton,
                const SkinnedMeshRenderer& smr)
{
    chain.nodes.clear();
    chain.builtSkeleton = &skeleton;

    const int rootNode = FindNode(skeleton, chain.rootBoneName);
    if (rootNode < 0) return;

    struct Pending { int node; int parentState; int depth; };
    std::vector<Pending> queue;
    queue.push_back({ rootNode, -1, 0 });

    for (std::size_t head = 0; head < queue.size(); ++head) {
        const Pending current = queue[head];
        if (current.node < 0 || current.node >= static_cast<int>(skeleton.nodes.size()))
            continue;
        if (current.node >= static_cast<int>(smr.nodeEntities.size()))
            continue;

        SpringBoneNodeState state{};
        state.nodeIndex   = current.node;
        state.parentState = current.parentState;
        const int stateIndex = static_cast<int>(chain.nodes.size());
        chain.nodes.push_back(state);

        const bool depthReached =
            chain.maxDepth > 0 && current.depth + 1 >= chain.maxDepth;
        if (depthReached) continue;

        for (int child : skeleton.nodes[static_cast<std::size_t>(current.node)].children)
            queue.push_back({ child, stateIndex, current.depth + 1 });
    }
}

/// @note boneAxis / boneLength は FK 姿勢で一度だけ確定し、揺れ中の骨長変化を防ぐ。
/// @note 葉は親からの向きへ仮想先端を伸ばす。
void InitializeChainRestAxes(SpringBoneChain& chain,
                             const std::vector<FkSnapshot>& fk)
{
    const std::size_t count = chain.nodes.size();
    for (std::size_t i = 0; i < count; ++i) {
        SpringBoneNodeState& node = chain.nodes[i];
        if (node.boneLength > 0.0f) continue;

        int firstChildState = -1;
        for (std::size_t j = i + 1; j < count; ++j) {
            if (chain.nodes[j].parentState == static_cast<int>(i)) {
                firstChildState = static_cast<int>(j);
                break;
            }
        }

        math::Vector3 worldTail;
        if (firstChildState >= 0) {
            worldTail = fk[static_cast<std::size_t>(firstChildState)].position;
        } else {
            /// @note 葉: 親からこのボーンへ向かう向きをそのまま延長する。親が居なければ真下。
            const math::Vector3 inherited = node.parentState >= 0
                ? (fk[i].position - fk[static_cast<std::size_t>(node.parentState)].position)
                : math::Vector3::ZERO;
            const math::Vector3 direction =
                inherited.NormalizedOr(math::Vector3{ 0.0f, -1.0f, 0.0f });
            worldTail = fk[i].position +
                direction * std::max(chain.leafTailLength, 0.001f);
        }

        const math::Vector3 delta = worldTail - fk[i].position;
        node.boneLength = delta.Length();
        if (node.boneLength <= math::EPSILON) {
            node.boneLength = 0.0f;
            continue;
        }
        node.boneAxis    = fk[i].rotation.Inverse() * (delta / node.boneLength);
        node.currentTail = worldTail;
        node.prevTail    = worldTail;
    }
}

/// @note テレポート後と初回に慣性を消し、静止姿勢へ戻す。
void ResetChainToRest(SpringBoneChain& chain, const std::vector<FkSnapshot>& fk)
{
    for (std::size_t i = 0; i < chain.nodes.size(); ++i) {
        SpringBoneNodeState& node = chain.nodes[i];
        if (node.boneLength <= 0.0f) continue;
        const math::Vector3 tail =
            fk[i].position + (fk[i].rotation * node.boneAxis) * node.boneLength;
        node.currentTail = tail;
        node.prevTail    = tail;
    }
}

/// @note Sphere は a == b の退化として扱う。
math::Vector3 ClosestPointOnSegment(const math::Vector3& a,
                                    const math::Vector3& b,
                                    const math::Vector3& p)
{
    const math::Vector3 ab = b - a;
    const float lengthSq = ab.LengthSq();
    if (lengthSq <= math::EPSILON) return a;
    const float t = math::Clamp01(math::Vector3::Dot(p - a, ab) / lengthSq);
    return a + ab * t;
}

math::Vector3 ResolveCollisions(const math::Vector3& tail,
                                float tailRadius,
                                const std::vector<ResolvedCollider>& colliders)
{
    math::Vector3 result = tail;
    for (const ResolvedCollider& collider : colliders) {
        const math::Vector3 closest = collider.isCapsule
            ? ClosestPointOnSegment(collider.start, collider.end, result)
            : collider.start;
        const math::Vector3 delta = result - closest;
        const float combined = collider.radius + tailRadius;
        const float distanceSq = delta.LengthSq();
        if (distanceSq >= combined * combined || distanceSq <= math::EPSILON) continue;
        const float distance = std::sqrt(distanceSq);
        result = closest + (delta / distance) * combined;
    }
    return result;
}

math::Vector3 ClampToCone(const math::Vector3& direction,
                          const math::Vector3& restDirection,
                          float maxAngleRadians)
{
    const float cosine =
        math::Clamp(math::Vector3::Dot(direction, restDirection), -1.0f, 1.0f);
    const float angle = std::acos(cosine);
    if (angle <= maxAngleRadians) return direction;

    math::Vector3 axis = math::Vector3::Cross(restDirection, direction);
    if (axis.LengthSq() <= math::EPSILON * math::EPSILON)
        axis = ArbitraryPerpendicular(restDirection);
    return (math::Quaternion::FromAxisAngle(axis.NormalizedOr(math::Vector3::UP),
                                            maxAngleRadians) * restDirection)
        .NormalizedOr(restDirection);
}

/// @pre nodes は親から子の順。確定した親姿勢を子の入力として使う。
void StepChain(SpringBoneChain& chain,
               const std::vector<FkSnapshot>& fk,
               const std::vector<ResolvedCollider>& colliders,
               float dt)
{
    const float stiffness = math::Clamp01(chain.stiffness);
    const float damping   = math::Clamp01(chain.damping);
    const float maxAngle  = chain.limitAngle > 0.0f
        ? math::Clamp(chain.limitAngle, 0.0f, 180.0f) * math::DEG2RAD
        : 0.0f;

    for (std::size_t i = 0; i < chain.nodes.size(); ++i) {
        SpringBoneNodeState& node = chain.nodes[i];
        if (node.boneLength <= 0.0f) {
            node.simPosition = fk[i].position;
            node.simRotation = fk[i].rotation;
            continue;
        }

        /// @note 根は FK のワールド姿勢、子は親の揺れ結果を基準にする。
        math::Vector3    position;
        math::Quaternion restRotation;
        if (node.parentState >= 0) {
            const SpringBoneNodeState& parent =
                chain.nodes[static_cast<std::size_t>(node.parentState)];
            position     = parent.simPosition + parent.simRotation * fk[i].localOffset;
            restRotation = (parent.simRotation * fk[i].localRotation).Normalized();
        } else {
            position     = fk[i].position;
            restRotation = fk[i].rotation;
        }

        const math::Vector3 restDirection =
            (restRotation * node.boneAxis).NormalizedOr(math::Vector3{ 0.0f, -1.0f, 0.0f });

        /// @note dt=0 で慣性を進めるとポーズ中も髪が流れるため、親姿勢への追従だけを行う。
        const math::Vector3 inertia = dt > 0.0f
            ? (node.currentTail - node.prevTail) * (1.0f - damping)
            : math::Vector3::ZERO;
        const math::Vector3 external =
            chain.gravityDirection.NormalizedOr(math::Vector3::ZERO) *
            (chain.gravityPower * dt * dt);

        math::Vector3 nextTail = node.currentTail + inertia + external;

        /// @note 方向ベクトルの加算では長い骨ほど戻りが鈍くなるため、先端を静止位置へ寄せる。
        const math::Vector3 restTail = position + restDirection * node.boneLength;
        nextTail = math::Vector3::Lerp(nextTail, restTail,
                                       math::Clamp01(stiffness * dt / kSubStep));

        math::Vector3 direction = (nextTail - position).NormalizedOr(restDirection);
        if (maxAngle > 0.0f) direction = ClampToCone(direction, restDirection, maxAngle);
        nextTail = position + direction * node.boneLength;

        nextTail = ResolveCollisions(nextTail, std::max(chain.radius, 0.0f), colliders);
        direction = (nextTail - position).NormalizedOr(restDirection);
        nextTail  = position + direction * node.boneLength;

        node.prevTail    = node.currentTail;
        node.currentTail = nextTail;
        node.simPosition = position;
        node.simRotation =
            (FromToRotation(restDirection, direction) * restRotation).Normalized();
    }
}

}

/// @note 骨 Transform の書き込みと smr の読み取りを宣言し、骨姿勢システム間の並列実行を防ぐ。
/// @see AnimatorSystem::GetAccess
ComponentAccess SpringBoneSystem::GetAccess() const
{
    return ComponentAccess{}
        .Reads<SkinnedMeshRenderer>()
        .Writes<SpringBoneComponent, AnimatorComponent, BoneComponent, Transform>();
}

OrderingHints SpringBoneSystem::GetOrder() const
{
    /// @note IK が骨を動かした「後」の姿勢が揺れの静止姿勢になる。逆順だと IK が揺れを打ち消す。
    return OrderingHints{}.After<IKSystem>();
}

void SpringBoneSystem::Update(SystemContext& ctx)
{
    if (!ctx.resources) return;
    FBZZ_PROFILE_SCOPE("SpringBoneSystem");

    Scene& scene = ctx.scene;
    renderer::ResourceManager& resources = *ctx.resources;

    const auto span     = scene.GetEntities<SpringBoneComponent>();
    const auto entities = std::vector<EntityID>(span.begin(), span.end());

    std::vector<FkSnapshot>       fk;
    std::vector<ResolvedCollider> colliders;

    for (EntityID id : entities) {
        GameObject* go = scene.GetGameObject(id);
        if (!go || !go->activeInHierarchy()) continue;

        auto* spring = go->GetComponent<SpringBoneComponent>();
        if (!spring) continue;
        ++spring->runtimeUpdateCount;
        spring->runtimeSimulatedBoneCount  = 0;
        spring->runtimeActiveColliderCount = 0;
        if (!spring->enabled) continue;
        if (!ctx.playing && !spring->simulateInEditor) continue;

        auto* animator = go->GetComponent<AnimatorComponent>();
        auto* smr      = bone_pose_detail::FindSkinnedMeshRenderer(*go);
        if (!animator || !smr || !smr->model || !smr->model->skeleton) continue;

        const asset::Skeleton& skeleton = *smr->model->skeleton;
        const math::Matrix4 ownerInv =
            math::Matrix4::Inverse(go->transform.GetWorldMatrix());

        const math::Vector3 ownerPosition = go->transform.worldPosition;
        const bool teleported = spring->hasLastOwnerPosition &&
            (ownerPosition - spring->lastOwnerPosition).Length() >
                std::max(spring->teleportResetDistance, 0.0f);
        const bool forceRest = teleported || !spring->hasLastOwnerPosition;
        spring->lastOwnerPosition    = ownerPosition;
        spring->hasLastOwnerPosition = true;

        colliders.clear();
        for (const SpringBoneCollider& collider : spring->colliders) {
            if (!collider.enabled || collider.radius <= 0.0f) continue;
            math::Vector3    basePosition = go->transform.worldPosition;
            math::Quaternion baseRotation = go->transform.worldRotation;
            math::Vector3    baseScale    = go->transform.worldScale;
            if (!collider.boneName.empty()) {
                const int node = FindNode(skeleton, collider.boneName);
                GameObject* bone = bone_pose_detail::BoneObject(scene, *smr, node);
                if (!bone) continue;
                basePosition = bone->transform.worldPosition;
                baseRotation = bone->transform.worldRotation;
                baseScale    = bone->transform.worldScale;
            }
            /// @note 半径にはスケールの最大成分を掛ける。非一様スケールの正確な楕円体は扱わない。
            const float scaleFactor =
                std::max({ std::abs(baseScale.x), std::abs(baseScale.y), std::abs(baseScale.z) });

            ResolvedCollider resolved{};
            resolved.start = basePosition +
                baseRotation * ComponentScale(collider.offset, baseScale);
            resolved.isCapsule = collider.shape == SpringBoneColliderShape::Capsule;
            resolved.end = resolved.isCapsule
                ? basePosition + baseRotation * ComponentScale(collider.tailOffset, baseScale)
                : resolved.start;
            resolved.radius = collider.radius * scaleFactor;
            colliders.push_back(resolved);
        }
        spring->runtimeActiveColliderCount = static_cast<int>(colliders.size());

        const float frameDt = std::max(ctx.dt, 0.0f);
        const int   subSteps = frameDt <= 0.0f
            ? 0
            : std::min(kMaxSubSteps,
                       std::max(1, static_cast<int>(std::ceil(frameDt / kSubStep))));
        const float stepDt = subSteps > 0
            ? std::min(frameDt / static_cast<float>(subSteps), kSubStep)
            : 0.0f;

        bool anyChainSolved = false;
        for (SpringBoneChain& chain : spring->chains) {
            if (!chain.enabled || chain.rootBoneName.empty()) continue;
            if (chain.builtSkeleton != &skeleton || chain.nodes.empty())
                BuildChain(chain, skeleton, *smr);
            if (chain.nodes.empty()) continue;

            /// @note FK 姿勢の凍結。以降 Transform は読まず、このスナップショットだけを入力にする。
            fk.assign(chain.nodes.size(), FkSnapshot{});
            bool resolved = true;
            for (std::size_t i = 0; i < chain.nodes.size(); ++i) {
                GameObject* bone = bone_pose_detail::BoneObject(scene, *smr, chain.nodes[i].nodeIndex);
                if (!bone) { resolved = false; break; }
                fk[i].position = bone->transform.worldPosition;
                fk[i].rotation = bone->transform.worldRotation;
                fk[i].scale    = bone->transform.worldScale;
            }
            if (!resolved) continue;

            for (std::size_t i = 0; i < chain.nodes.size(); ++i) {
                const int parentState = chain.nodes[i].parentState;
                if (parentState < 0) continue;
                const std::size_t parent = static_cast<std::size_t>(parentState);
                const math::Quaternion parentInverse = fk[parent].rotation.Inverse();
                fk[i].localOffset   = parentInverse * (fk[i].position - fk[parent].position);
                fk[i].localRotation = (parentInverse * fk[i].rotation).Normalized();
            }

            InitializeChainRestAxes(chain, fk);
            if (forceRest) ResetChainToRest(chain, fk);

            for (int step = 0; step < subSteps; ++step)
                StepChain(chain, fk, colliders, stepDt);
            if (subSteps == 0) {
                /// @note dt が 0 (ポーズ中など) でも、親の姿勢変化へは追従させる。
                StepChain(chain, fk, colliders, 0.0f);
            }

            const float chainWeight = math::Clamp01(chain.weight);
            if (chainWeight <= 0.0f) continue;

            for (std::size_t i = 0; i < chain.nodes.size(); ++i) {
                const SpringBoneNodeState& node = chain.nodes[i];
                if (node.nodeIndex < 0) continue;
                const math::Vector3 position =
                    math::Vector3::Lerp(fk[i].position, node.simPosition, chainWeight);
                const math::Quaternion rotation =
                    math::Quaternion::Slerp(fk[i].rotation, node.simRotation, chainWeight);
                bone_pose_detail::CommitBoneWorldPose(scene, skeleton, *smr, *animator, ownerInv,
                                    node.nodeIndex, position, rotation, fk[i].scale);
                ++spring->runtimeSimulatedBoneCount;
            }
            anyChainSolved = true;
        }

        if (anyChainSolved && animator->skinningBuffer.IsValid())
            UploadBoneMatrices(*animator, resources);
    }
}

}
