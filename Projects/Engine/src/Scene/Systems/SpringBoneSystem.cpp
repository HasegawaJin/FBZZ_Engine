/// @file SpringBoneSystem.cpp
/// @brief 揺れもの (二次モーション) の Verlet 積分と骨行列への書き戻し
/// @author Hasegawa Jin
/// @date 2026-08-25
#include <Engine/Scene/Systems/SpringBoneSystem.hpp>

#include "Engine/Core/Scheduler/SystemContext.hpp"
#include "Engine/Scene/Systems/AnimatorSystem.hpp"
#include "Engine/Scene/Systems/IKSystem.hpp"
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/BoneComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/SpringBoneComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
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

// 揺れの積分は dt に敏感で、フレーム落ちの 1 回で髪が伸び切って戻らなくなる。
// 見た目が変わらない範囲で刻みを固定し、余ったぶんは次フレームへ持ち越さず捨てる。
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

int FindNode(const asset::Skeleton& skeleton, const std::string& boneName)
{
    if (boneName.empty()) return -1;
    if (const auto exact = skeleton.nodeMap.find(boneName); exact != skeleton.nodeMap.end())
        return exact->second;
    for (std::size_t i = 0; i < skeleton.nodes.size(); ++i) {
        if (CanonicalBoneName(skeleton.nodes[i].name) == boneName)
            return static_cast<int>(i);
    }
    return -1;
}

GameObject* BoneObject(Scene& scene, const SkinnedMeshRenderer& smr, int nodeIndex)
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(smr.nodeEntities.size()))
        return nullptr;
    return scene.GetGameObject(smr.nodeEntities[static_cast<std::size_t>(nodeIndex)]);
}

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

// IKSystem::CommitBoneWorldPose と同じ規約。スキニング行列とボーン Transform を対で更新する。
// どちらか片方だけを書くと、メッシュと「骨に付いているもの」が食い違う。
void CommitBoneWorldPose(Scene& scene,
                         const asset::Skeleton& skeleton,
                         const SkinnedMeshRenderer& smr,
                         AnimatorComponent& animator,
                         const math::Matrix4& ownerInv,
                         int nodeIndex,
                         const math::Vector3& worldPosition,
                         const math::Quaternion& worldRotation,
                         const math::Vector3& worldScale)
{
    if (nodeIndex < 0 ||
        nodeIndex >= static_cast<int>(smr.nodeEntities.size()) ||
        nodeIndex >= static_cast<int>(animator.nodeGlobalTransforms.size()))
        return;

    const math::Matrix4 rootTransform =
        math::Matrix4::Inverse(skeleton.rootInverseTransform);
    animator.nodeGlobalTransforms[static_cast<std::size_t>(nodeIndex)] =
        rootTransform * ownerInv *
        math::Matrix4::TRS(worldPosition, worldRotation, worldScale);
    RecalcBoneMatrix(skeleton, animator.nodeGlobalTransforms, animator.boneMatrices, nodeIndex);

    if (GameObject* bone = BoneObject(scene, smr, nodeIndex))
        SetWorldPose(*bone, worldPosition, worldRotation, worldScale);
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

// このフレームの FK/IK ワールド姿勢。積分中に Transform を読み直さないための凍結値。
struct FkSnapshot {
    math::Vector3    position = math::Vector3::ZERO;
    math::Quaternion rotation = math::Quaternion::Identity();
    math::Vector3    scale    = math::Vector3::ONE;
    // チェーン内の親から見たローカル姿勢。親の揺れ結果へ乗せ直すために使う。
    math::Vector3    localOffset   = math::Vector3::ZERO;
    math::Quaternion localRotation = math::Quaternion::Identity();
};

// ワールド空間へ展開した衝突形状。
struct ResolvedCollider {
    math::Vector3 start  = math::Vector3::ZERO;
    math::Vector3 end    = math::Vector3::ZERO;
    float         radius = 0.0f;
    bool          isCapsule = false;
};

// 根から幅優先で辿り、親が必ず先に来る順で nodes を組む。
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

// boneAxis / boneLength を FK 姿勢から 1 度だけ確定する。
// 子を持つノードは最初の子へ、葉は親からの向きへ仮想先端を伸ばす。
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
            // 葉: 親からこのボーンへ向かう向きをそのまま延長する。親が居なければ真下。
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

// 静止姿勢へ強制的に戻す。テレポート後と初回に使う。
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

// 線分 (a,b) 上で p に最も近い点。Sphere は a == b の退化として扱う。
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

// 衝突形状の外へ押し出した先端位置を返す。
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

// 静止方向から maxAngle 以内へ先端方向を制限する。
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

// 1 サブステップぶんの積分。親から順に解き、確定姿勢を子の入力にする。
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

        // 親がチェーン内なら、その揺れ結果へローカル姿勢を乗せ直す。
        // 根 (parentState < 0) は FK のワールド姿勢をそのまま基準にする。
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

        // dt=0 (ポーズ・ステップ実行) では慣性を進めない。進めると止めた瞬間から
        // 髪だけが流れ続け、コマ送りで姿勢を確認できなくなる。
        const math::Vector3 inertia = dt > 0.0f
            ? (node.currentTail - node.prevTail) * (1.0f - damping)
            : math::Vector3::ZERO;
        const math::Vector3 external =
            chain.gravityDirection.NormalizedOr(math::Vector3::ZERO) *
            (chain.gravityPower * dt * dt);

        math::Vector3 nextTail = node.currentTail + inertia + external;

        // 復元は「先端を静止位置へ寄せる」形で入れる。VRM 系のように方向ベクトルを
        // 足す形だとボーン長で効き方が変わり、長い髪ほど戻りが鈍くなる。
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

} // namespace

ComponentAccess SpringBoneSystem::GetAccess() const
{
    return ComponentAccess{}
        .Writes<SpringBoneComponent, AnimatorComponent, BoneComponent>();
}

OrderingHints SpringBoneSystem::GetOrder() const
{
    // IK が骨を動かした「後」の姿勢が揺れの静止姿勢になる。逆順だと IK が揺れを打ち消す。
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
        auto* smr      = FindSkinnedMeshRenderer(*go);
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
                GameObject* bone = BoneObject(scene, *smr, node);
                if (!bone) continue;
                basePosition = bone->transform.worldPosition;
                baseRotation = bone->transform.worldRotation;
                baseScale    = bone->transform.worldScale;
            }
            // 半径にはスケールの最大成分を掛ける。非一様スケールの正確な楕円体は扱わない。
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

            // FK 姿勢の凍結。以降 Transform は読まず、このスナップショットだけを入力にする。
            fk.assign(chain.nodes.size(), FkSnapshot{});
            bool resolved = true;
            for (std::size_t i = 0; i < chain.nodes.size(); ++i) {
                GameObject* bone = BoneObject(scene, *smr, chain.nodes[i].nodeIndex);
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
                // dt が 0 (ポーズ中など) でも、親の姿勢変化へは追従させる。
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
                CommitBoneWorldPose(scene, skeleton, *smr, *animator, ownerInv,
                                    node.nodeIndex, position, rotation, fk[i].scale);
                ++spring->runtimeSimulatedBoneCount;
            }
            anyChainSolved = true;
        }

        if (anyChainSolved && animator->skinningBuffer.IsValid())
            UploadBoneMatrices(*animator, resources);
    }
}

} // namespace fbzz::scene
