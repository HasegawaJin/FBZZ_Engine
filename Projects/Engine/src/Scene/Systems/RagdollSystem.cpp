/// @file    RagdollSystem.cpp
/// @brief   骨を質点系として落とし、確定済みポーズへブレンドして書き戻す
/// @author  Hasegawa Jin
/// @date    2026-09-01
#include <Engine/Scene/Systems/RagdollSystem.hpp>

#include "Engine/Core/Scheduler/SystemContext.hpp"
#include "Engine/Scene/Systems/SpringBoneSystem.hpp"
#include <Engine/Asset/Skeleton.hpp>
#include <Engine/Profiler/ProfileScope.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/BoneComponent.hpp>
#include <Engine/Scene/Components/RagdollComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
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

// 崩れ方は刻みに敏感で、フレーム落ちの 1 回で脚が伸び切る。SpringBoneSystem と
// 同じ刻みに固定し、余ったぶんは持ち越さず捨てる。
constexpr float kSubStep     = 1.0f / 60.0f;
constexpr int   kMaxSubSteps = 4;

// WHY SpringBoneSystem の同名ヘルパーを共有しないか:
//   あちらはすべて無名名前空間のファイルローカルで、IKSystem との間でも
//   CommitBoneWorldPose を «同じ規約» と書いて複製している。骨を書く系の
//   ヘルパーを共有ヘッダーへ出すのは 3 つ目の書き手が出た今でも早く、
//   規約が変わったときに 3 箇所が同時に壊れる方が発見が早い。
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
    if (boneName.empty()) return skeleton.rootNodeIndex;
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

// SpringBoneSystem::CommitBoneWorldPose と同じ規約。スキニング行列とボーン Transform を
// 対で更新する。どちらか片方だけを書くと、メッシュと「骨に付いているもの」が食い違う。
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

/// このフレームの FK/IK/揺れ込みのワールド姿勢。ブレンド先であり、捕獲元でもある。
struct FkSnapshot {
    math::Vector3    position = math::Vector3::ZERO;
    math::Quaternion rotation = math::Quaternion::Identity();
    math::Vector3    scale    = math::Vector3::ONE;
};

// 根から幅優先で辿り、親が必ず先に来る順で particles を組む。
void BuildParticles(RagdollComponent& ragdoll,
                    const asset::Skeleton& skeleton,
                    const SkinnedMeshRenderer& smr)
{
    ragdoll.particles.clear();
    ragdoll.links.clear();
    ragdoll.builtSkeleton = &skeleton;

    const int rootNode = FindNode(skeleton, ragdoll.rootBoneName);
    if (rootNode < 0) return;

    struct Pending { int node; int parentParticle; int depth; };
    std::vector<Pending> queue;
    queue.push_back({ rootNode, -1, 0 });

    for (std::size_t head = 0; head < queue.size(); ++head) {
        const Pending current = queue[head];
        if (current.node < 0 || current.node >= static_cast<int>(skeleton.nodes.size()))
            continue;
        if (current.node >= static_cast<int>(smr.nodeEntities.size()))
            continue;

        RagdollParticle particle{};
        particle.nodeIndex      = current.node;
        particle.parentParticle = current.parentParticle;
        const int index = static_cast<int>(ragdoll.particles.size());
        ragdoll.particles.push_back(particle);

        const bool depthReached =
            ragdoll.maxDepth > 0 && current.depth + 1 >= ragdoll.maxDepth;
        if (depthReached) continue;

        for (int child : skeleton.nodes[static_cast<std::size_t>(current.node)].children)
            queue.push_back({ child, index, current.depth + 1 });
    }

    // 最初の子は回転を作る «向きの相手»。親が先に並んでいるので前方向の走査で足りる。
    for (std::size_t i = 0; i < ragdoll.particles.size(); ++i) {
        const int parent = ragdoll.particles[i].parentParticle;
        if (parent < 0) continue;
        RagdollParticle& parentParticle = ragdoll.particles[static_cast<std::size_t>(parent)];
        if (parentParticle.firstChild < 0)
            parentParticle.firstChild = static_cast<int>(i);
    }
}

// 捕獲した姿勢から距離拘束を張る。
//
// WHY 親子だけでなく «筋交い» も張るか: 親子の距離だけでは関節が自由に折れる紐に
//     なり、四足ロボが布のように潰れる。祖父と孫を結ぶと «曲がりにくさ» が
//     距離拘束だけで表現でき、角度拘束を実装せずに関節体らしい崩れ方になる。
void BuildLinks(RagdollComponent& ragdoll, const std::vector<FkSnapshot>& capture)
{
    ragdoll.links.clear();
    const std::size_t count = ragdoll.particles.size();
    const float brace = math::Clamp01(ragdoll.braceStiffness);

    for (std::size_t i = 0; i < count; ++i) {
        const int parent = ragdoll.particles[i].parentParticle;
        if (parent < 0) continue;
        const std::size_t p = static_cast<std::size_t>(parent);

        RagdollLink bone{};
        bone.a = parent;
        bone.b = static_cast<int>(i);
        bone.restLength = (capture[i].position - capture[p].position).Length();
        bone.stiffness  = 1.0f;
        if (bone.restLength > math::EPSILON) ragdoll.links.push_back(bone);

        const int grand = ragdoll.particles[p].parentParticle;
        if (grand < 0 || brace <= 0.0f) continue;
        const std::size_t g = static_cast<std::size_t>(grand);

        RagdollLink support{};
        support.a = grand;
        support.b = static_cast<int>(i);
        support.restLength = (capture[i].position - capture[g].position).Length();
        support.stiffness  = brace;
        if (support.restLength > math::EPSILON) ragdoll.links.push_back(support);
    }
}

// 捕獲。ここが «アニメーションから物理へ» の境目で、以降 FK は参照しない。
//
// WHY 前フレームの骨から初速を取らないか: 慣性を継ぐには全フレーム骨を控え続ける
//     必要があり、倒れていない間もコストを払うことになる。しかも継いだ初速は
//     クリップ次第で毎回違い、演出として «どちらへ倒したいか» を作れない。
//     初速は 0 から始め、倒す方向は呼び出し側が Push で明示する。
void Capture(RagdollComponent& ragdoll, const std::vector<FkSnapshot>& capture)
{
    for (std::size_t i = 0; i < ragdoll.particles.size(); ++i) {
        RagdollParticle& particle = ragdoll.particles[i];
        particle.position     = capture[i].position;
        particle.prevPosition = capture[i].position;
        particle.simPosition  = capture[i].position;
        particle.simRotation  = capture[i].rotation;
        particle.captureRotation = capture[i].rotation;
        particle.captureScale    = capture[i].scale;
        particle.invMass         = 1.0f;

        if (particle.firstChild >= 0) {
            const std::size_t c = static_cast<std::size_t>(particle.firstChild);
            const math::Vector3 delta = capture[c].position - capture[i].position;
            particle.restDirection = delta.NormalizedOr(math::Vector3::ZERO);
            particle.hasRest = delta.LengthSq() > math::EPSILON * math::EPSILON;
        } else {
            particle.restDirection = math::Vector3::ZERO;
            particle.hasRest = false;
        }
    }
    BuildLinks(ragdoll, capture);
}

void ApplyImpulses(RagdollComponent& ragdoll, float stepDt)
{
    if (ragdoll.pendingImpulses.empty() || stepDt <= 0.0f) return;

    for (const RagdollImpulse& impulse : ragdoll.pendingImpulses) {
        for (RagdollParticle& particle : ragdoll.particles) {
            if (particle.invMass <= 0.0f) continue;
            float scale = 1.0f;
            if (impulse.radius > 0.0f) {
                const float distance = (particle.position - impulse.origin).Length();
                scale = 1.0f - math::Clamp01(distance / impulse.radius);
                if (scale <= 0.0f) continue;
            }
            // Verlet では速度が (現在 - 前) に埋まっている。前の位置を後ろへずらす。
            particle.prevPosition -= impulse.velocity * (scale * stepDt);
        }
    }
    ragdoll.pendingImpulses.clear();
}

void SolveGround(RagdollComponent& ragdoll)
{
    const float floor    = ragdoll.groundHeight + std::max(ragdoll.boneRadius, 0.0f);
    const float friction = math::Clamp01(ragdoll.groundFriction);

    for (RagdollParticle& particle : ragdoll.particles) {
        if (particle.invMass <= 0.0f || particle.position.y >= floor) continue;
        particle.position.y = floor;

        // 接地したコマだけ水平の «前フレーム» を現在へ寄せる ＝ 速度を削る。
        const math::Vector3 velocity = particle.position - particle.prevPosition;
        particle.prevPosition.x = particle.position.x - velocity.x * (1.0f - friction);
        particle.prevPosition.z = particle.position.z - velocity.z * (1.0f - friction);
        if (particle.prevPosition.y < particle.position.y)
            particle.prevPosition.y = particle.position.y;
    }
}

void SolveLinks(RagdollComponent& ragdoll)
{
    const int iterations = std::clamp(ragdoll.iterations, 1, 32);
    for (int iteration = 0; iteration < iterations; ++iteration) {
        for (const RagdollLink& link : ragdoll.links) {
            if (link.a < 0 || link.b < 0) continue;
            RagdollParticle& a = ragdoll.particles[static_cast<std::size_t>(link.a)];
            RagdollParticle& b = ragdoll.particles[static_cast<std::size_t>(link.b)];

            const float invSum = a.invMass + b.invMass;
            if (invSum <= 0.0f) continue;

            const math::Vector3 delta = b.position - a.position;
            const float distance = delta.Length();
            if (distance <= math::EPSILON) continue;

            const math::Vector3 correction =
                delta * (((distance - link.restLength) / distance) * link.stiffness / invSum);
            a.position += correction * a.invMass;
            b.position -= correction * b.invMass;
        }
        SolveGround(ragdoll);
    }
}

void Integrate(RagdollComponent& ragdoll, float stepDt)
{
    const float damping = math::Clamp01(ragdoll.damping);
    const float gravity =
        std::max(ragdoll.gravity, 0.0f) * std::max(ragdoll.activationGravity, 0.0f);
    const math::Vector3 fall{ 0.0f, -gravity * stepDt * stepDt, 0.0f };

    for (RagdollParticle& particle : ragdoll.particles) {
        if (particle.invMass <= 0.0f) continue;
        const math::Vector3 velocity = (particle.position - particle.prevPosition) * (1.0f - damping);
        particle.prevPosition = particle.position;
        particle.position += velocity + fall;
    }
}

// 質点の位置から骨の回転を組み直す。親が先に並んでいるので 1 パスで解ける。
void ResolveRotations(RagdollComponent& ragdoll, std::vector<math::Quaternion>& deltas)
{
    deltas.assign(ragdoll.particles.size(), math::Quaternion::Identity());

    for (std::size_t i = 0; i < ragdoll.particles.size(); ++i) {
        RagdollParticle& particle = ragdoll.particles[i];

        math::Quaternion delta = math::Quaternion::Identity();
        if (particle.hasRest && particle.firstChild >= 0) {
            const std::size_t c = static_cast<std::size_t>(particle.firstChild);
            const math::Vector3 direction =
                (ragdoll.particles[c].position - particle.position)
                    .NormalizedOr(particle.restDirection);
            delta = FromToRotation(particle.restDirection, direction);
        } else if (particle.parentParticle >= 0) {
            // 葉は自分の向きを持たない。親の «傾いたぶん» をそのまま引き継ぐ。
            delta = deltas[static_cast<std::size_t>(particle.parentParticle)];
        }

        deltas[i] = delta;
        particle.simPosition = particle.position;
        particle.simRotation = (delta * particle.captureRotation).Normalized();
    }
}

// 段階を進め、このフレームの適用率を返す。
float AdvancePhase(RagdollComponent& ragdoll, float dt)
{
    const float ceiling = math::Clamp01(ragdoll.activationWeight);

    switch (ragdoll.phase) {
    case RagdollPhase::BlendIn: {
        ragdoll.phaseTimer += dt;
        const float duration = std::max(ragdoll.blendIn, 0.0f);
        if (duration <= 0.0f || ragdoll.phaseTimer >= duration) {
            ragdoll.phase = RagdollPhase::Hold;
            ragdoll.phaseTimer = 0.0f;
            ragdoll.weight = ceiling;
        } else {
            ragdoll.weight = ceiling * math::Clamp01(ragdoll.phaseTimer / duration);
        }
        break;
    }
    case RagdollPhase::Hold: {
        ragdoll.weight = ceiling;
        if (ragdoll.holdRemaining > 0.0f) {
            ragdoll.holdRemaining -= dt;
            if (ragdoll.holdRemaining <= 0.0f) ragdoll.endRequested = true;
        }
        break;
    }
    case RagdollPhase::BlendOut: {
        ragdoll.phaseTimer += dt;
        const float duration = std::max(ragdoll.blendOut, 0.0f);
        if (duration <= 0.0f || ragdoll.phaseTimer >= duration) {
            ragdoll.phase  = RagdollPhase::Idle;
            ragdoll.weight = 0.0f;
            ragdoll.phaseTimer = 0.0f;
        } else {
            ragdoll.weight = ceiling * (1.0f - math::Clamp01(ragdoll.phaseTimer / duration));
        }
        break;
    }
    case RagdollPhase::Idle:
    default:
        ragdoll.weight = 0.0f;
        break;
    }
    return math::Clamp01(ragdoll.weight);
}

} // namespace

ComponentAccess RagdollSystem::GetAccess() const
{
    return ComponentAccess{}
        .Writes<RagdollComponent, AnimatorComponent, BoneComponent>();
}

OrderingHints RagdollSystem::GetOrder() const
{
    // 揺れものが確定させた姿勢がブレンド先になる。逆順だと戻り際に揺れが物理を打ち消す。
    return OrderingHints{}.After<SpringBoneSystem>();
}

void RagdollSystem::Update(SystemContext& ctx)
{
    if (!ctx.resources) return;
    FBZZ_PROFILE_SCOPE("RagdollSystem");

    Scene& scene = ctx.scene;
    renderer::ResourceManager& resources = *ctx.resources;

    const auto span     = scene.GetEntities<RagdollComponent>();
    const auto entities = std::vector<EntityID>(span.begin(), span.end());

    std::vector<FkSnapshot>       fk;
    std::vector<math::Quaternion> deltas;

    for (EntityID id : entities) {
        GameObject* go = scene.GetGameObject(id);
        if (!go || !go->activeInHierarchy()) continue;

        auto* ragdoll = go->GetComponent<RagdollComponent>();
        if (!ragdoll) continue;
        ragdoll->runtimeParticleCount = 0;
        ragdoll->runtimeLinkCount     = 0;

        if (!ragdoll->enabled || (!ctx.playing && !ragdoll->simulateInEditor)) {
            ragdoll->runtimeStatus = ragdoll->enabled
                ? RagdollStatus::NotPlaying
                : RagdollStatus::Disabled;
            ragdoll->phase  = RagdollPhase::Idle;
            ragdoll->weight = 0.0f;
            ragdoll->pendingImpulses.clear();
            ragdoll->beginRequested = false;
            ragdoll->endRequested   = false;
            continue;
        }

        // 止まっているあいだは骨を 1 本も触らない。Animator が書いた姿勢がそのまま残る。
        if (ragdoll->phase == RagdollPhase::Idle && !ragdoll->beginRequested) {
            ragdoll->runtimeStatus = RagdollStatus::Idle;
            ragdoll->pendingImpulses.clear();
            ragdoll->endRequested = false;
            continue;
        }

        auto* animator = go->GetComponent<AnimatorComponent>();
        if (!animator) {
            ragdoll->runtimeStatus  = RagdollStatus::NoAnimator;
            ragdoll->beginRequested = false;
            continue;
        }
        auto* smr = FindSkinnedMeshRenderer(*go);
        if (!smr || !smr->model || !smr->model->skeleton) {
            ragdoll->runtimeStatus  = RagdollStatus::NoSkinnedMesh;
            ragdoll->beginRequested = false;
            continue;
        }

        const asset::Skeleton& skeleton = *smr->model->skeleton;
        if (ragdoll->builtSkeleton != &skeleton || ragdoll->particles.empty())
            BuildParticles(*ragdoll, skeleton, *smr);
        if (ragdoll->particles.empty()) {
            ragdoll->runtimeStatus  = RagdollStatus::NoParticles;
            ragdoll->phase          = RagdollPhase::Idle;
            ragdoll->beginRequested = false;
            continue;
        }

        // FK 姿勢の凍結。ブレンド先であり、Begin したフレームでは捕獲元でもある。
        fk.assign(ragdoll->particles.size(), FkSnapshot{});
        bool resolved = true;
        for (std::size_t i = 0; i < ragdoll->particles.size(); ++i) {
            GameObject* bone = BoneObject(scene, *smr, ragdoll->particles[i].nodeIndex);
            if (!bone) { resolved = false; break; }
            fk[i].position = bone->transform.worldPosition;
            fk[i].rotation = bone->transform.worldRotation;
            fk[i].scale    = bone->transform.worldScale;
        }
        if (!resolved) {
            ragdoll->runtimeStatus  = RagdollStatus::NoBones;
            ragdoll->beginRequested = false;
            continue;
        }

        if (ragdoll->beginRequested) {
            ragdoll->beginRequested = false;
            ragdoll->endRequested   = false;
            ragdoll->groundHeight   = go->transform.worldPosition.y + ragdoll->groundOffset;
            ragdoll->phase          = RagdollPhase::BlendIn;
            ragdoll->phaseTimer     = 0.0f;
            ragdoll->weight         = 0.0f;
            Capture(*ragdoll, fk);
        }

        if (ragdoll->endRequested && ragdoll->phase != RagdollPhase::BlendOut) {
            ragdoll->endRequested = false;
            ragdoll->phase        = RagdollPhase::BlendOut;
            ragdoll->phaseTimer   = 0.0f;
        }

        const float frameDt = std::max(ctx.dt, 0.0f);
        const int   subSteps = frameDt <= 0.0f
            ? 0
            : std::min(kMaxSubSteps,
                       std::max(1, static_cast<int>(std::ceil(frameDt / kSubStep))));
        const float stepDt = subSteps > 0
            ? std::min(frameDt / static_cast<float>(subSteps), kSubStep)
            : 0.0f;

        ApplyImpulses(*ragdoll, stepDt > 0.0f ? stepDt : kSubStep);
        for (int step = 0; step < subSteps; ++step) {
            Integrate(*ragdoll, stepDt);
            SolveLinks(*ragdoll);
        }

        ResolveRotations(*ragdoll, deltas);

        const float weight = AdvancePhase(*ragdoll, frameDt);
        ragdoll->runtimeParticleCount = static_cast<int>(ragdoll->particles.size());
        ragdoll->runtimeLinkCount     = static_cast<int>(ragdoll->links.size());
        ragdoll->runtimeStatus        = RagdollStatus::Running;
        if (weight <= 0.0f) continue;

        const math::Matrix4 ownerInv =
            math::Matrix4::Inverse(go->transform.GetWorldMatrix());

        for (std::size_t i = 0; i < ragdoll->particles.size(); ++i) {
            const RagdollParticle& particle = ragdoll->particles[i];
            if (particle.nodeIndex < 0) continue;
            const math::Vector3 position =
                math::Vector3::Lerp(fk[i].position, particle.simPosition, weight);
            const math::Quaternion rotation =
                math::Quaternion::Slerp(fk[i].rotation, particle.simRotation, weight);
            CommitBoneWorldPose(scene, skeleton, *smr, *animator, ownerInv,
                                particle.nodeIndex, position, rotation, fk[i].scale);
        }

        if (animator->skinningBuffer.IsValid())
            UploadBoneMatrices(*animator, resources);
    }
}

} // namespace fbzz::scene
