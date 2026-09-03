/// @file    RagdollSystem.cpp
/// @brief   スケルトンと XPBD 関節体の橋渡し。捕獲・駆動・ブレンド・書き戻し
/// @author  Hasegawa Jin
/// @date    2026-09-01
///
/// 物理そのものは RagdollRig / XPBDSolver が持つ。ここがやるのは «骨の世界» と
/// «剛体の世界» の変換だけ ── どの骨を剛体にするか、今の姿勢をどう渡すか、
/// 返ってきた姿勢をどうスキニングへ書くか。
#include <Engine/Scene/Systems/RagdollSystem.hpp>

#include "Engine/Core/Scheduler/SystemContext.hpp"
#include "Engine/Scene/Systems/SpringBoneSystem.hpp"
#include <Engine/Asset/Skeleton.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Profiler/ProfileScope.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/BoneComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/RagdollComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
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
#include <memory>
#include <string>
#include <vector>

namespace fbzz::scene {

namespace {

struct SkinningCB {
    math::Matrix4 boneMatrices[asset::MAX_SKINNING_BONES];
};

// フレーム落ちの 1 回で関節体が弾け飛ばないための上限。余ったぶんは持ち越さず捨てる。
// substep 数は RagdollComponent::substeps が決めるので、ここは «1 回で進める最大» だけ。
constexpr float kMaxFrameStep = 1.0f / 30.0f;

// WHY SpringBoneSystem の同名ヘルパーを共有しないか:
//   あちらはすべて無名名前空間のファイルローカルで、IKSystem との間でも
//   CommitBoneWorldPose を «同じ規約» と書いて複製している。骨を書く系の
//   ヘルパーを共有ヘッダーへ出すのは 3 つ目の書き手が出た今でも早く、
//   規約が変わったときに 3 箇所が同時に壊れる方が発見が早い。
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

// AnimatorSystem::DecomposeAffineMatrix と同じ規約 (行優先、平行移動は 4 列目)。
void DecomposeAffine(const math::Matrix4& matrix,
                     math::Vector3& outPosition, math::Quaternion& outRotation)
{
    outPosition = { matrix.m[0][3], matrix.m[1][3], matrix.m[2][3] };

    math::Matrix4 rotation = math::Matrix4::Identity();
    for (int column = 0; column < 3; ++column) {
        const math::Vector3 basis{
            matrix.m[0][column], matrix.m[1][column], matrix.m[2][column]
        };
        const float length = basis.Length();
        const float inverse = length > math::EPSILON ? 1.0f / length : 0.0f;
        for (int row = 0; row < 3; ++row)
            rotation.m[row][column] = matrix.m[row][column] * inverse;
    }
    outRotation = math::Quaternion::FromMatrix4(rotation).Normalized();
}

// 根から幅優先で辿り、親が必ず子より前に来る順で骨の並びを作る。
// RagdollRig はこの順序を前提に «剛体を持たない骨を親から埋める» ので、崩すと葉が飛ぶ。
void BuildBoneList(RagdollComponent& ragdoll,
                   const asset::Skeleton& skeleton,
                   const SkinnedMeshRenderer& smr,
                   const RagdollProfile& profile)
{
    RagdollRuntime& runtime = ragdoll.runtime;
    runtime.bones.clear();
    runtime.boneNodes.clear();
    runtime.boneScales.clear();

    const int rootNode = FindNode(skeleton, ragdoll.rootBoneName);
    if (rootNode < 0) return;

    struct Pending { int node; int parent; int depth; };
    std::vector<Pending> queue;
    queue.push_back({ rootNode, -1, 0 });

    for (std::size_t head = 0; head < queue.size(); ++head) {
        const Pending current = queue[head];
        if (current.node < 0 || current.node >= static_cast<int>(skeleton.nodes.size()))
            continue;
        if (current.node >= static_cast<int>(smr.nodeEntities.size()))
            continue;

        RagdollBonePose bone;
        bone.parent = current.parent;
        bone.name   = CanonicalBoneName(skeleton.nodes[static_cast<std::size_t>(current.node)].name);
        const int index = static_cast<int>(runtime.bones.size());
        runtime.bones.push_back(std::move(bone));
        runtime.boneNodes.push_back(current.node);

        const bool depthReached =
            ragdoll.maxDepth > 0 && current.depth + 1 >= ragdoll.maxDepth;
        if (depthReached) continue;

        for (int child : skeleton.nodes[static_cast<std::size_t>(current.node)].children) {
            if (child < 0 || child >= static_cast<int>(skeleton.nodes.size())) continue;
            // プロファイルが落とす枝はここで切る。剛体を作らないのではなく骨ごと拾わない
            // ので、親のカプセルが «指へ向かう短い節» になるのも同時に防げる。
            if (profile.IsExcluded(
                    CanonicalBoneName(skeleton.nodes[static_cast<std::size_t>(child)].name)))
                continue;
            queue.push_back({ child, index, current.depth + 1 });
        }
    }

    runtime.boneScales.assign(runtime.bones.size(), math::Vector3::ONE);
}

// 全ノードのバインドポーズをスケルトン空間で組む。深さ優先なので親が先に確定する。
std::vector<math::Matrix4> BuildBindGlobals(const asset::Skeleton& skeleton)
{
    std::vector<math::Matrix4> globals(skeleton.nodes.size(), math::Matrix4::Identity());
    if (skeleton.rootNodeIndex < 0) return globals;

    std::vector<int> stack{ skeleton.rootNodeIndex };
    while (!stack.empty()) {
        const int node = stack.back();
        stack.pop_back();
        if (node < 0 || node >= static_cast<int>(skeleton.nodes.size())) continue;

        const asset::SkeletonNode& entry = skeleton.nodes[static_cast<std::size_t>(node)];
        globals[static_cast<std::size_t>(node)] =
            entry.parentIndex >= 0
                ? globals[static_cast<std::size_t>(entry.parentIndex)] * entry.localBindTransform
                : entry.localBindTransform;

        for (int child : entry.children) stack.push_back(child);
    }
    return globals;
}

// 剛体と関節を組む «たわみ 0» の基準姿勢。
//
// WHY 起動時のアニメーション姿勢ではなくバインドポーズか: 可動域は関節フレームからの
//     角度で測るので、基準が «たまたま再生していたクリップ» だと «膝は前へ 105°» が
//     毎回違う所から数え始めることになる。走っている途中で倒すと膝が伸び切ったまま
//     固まる、といった «起動したフレーム次第» の破綻がここから出る。
std::vector<RagdollBonePose> BuildBindPose(const RagdollRuntime&  runtime,
                                           const asset::Skeleton& skeleton,
                                           const math::Matrix4&   ownerWorld)
{
    const std::vector<math::Matrix4> globals = BuildBindGlobals(skeleton);
    const math::Matrix4 toWorld = ownerWorld * skeleton.rootInverseTransform;

    std::vector<RagdollBonePose> pose = runtime.bones;
    for (std::size_t i = 0; i < pose.size(); ++i) {
        const int node = runtime.boneNodes[i];
        if (node < 0 || node >= static_cast<int>(globals.size())) continue;
        DecomposeAffine(toWorld * globals[static_cast<std::size_t>(node)],
                        pose[i].position, pose[i].rotation);
    }
    return pose;
}

// このフレームの FK/IK/揺れ込みの姿勢を読む。捕獲元・サーボの目標・ブレンド先を兼ねる。
bool SampleFkPose(RagdollRuntime& runtime, Scene& scene, const SkinnedMeshRenderer& smr)
{
    for (std::size_t i = 0; i < runtime.bones.size(); ++i) {
        GameObject* bone = BoneObject(scene, smr, runtime.boneNodes[i]);
        if (!bone) return false;
        runtime.bones[i].position = bone->transform.worldPosition;
        runtime.bones[i].rotation = bone->transform.worldRotation;
        runtime.boneScales[i]     = bone->transform.worldScale;
    }
    return true;
}

// 剛体を組み直す条件。骨の並びが変わればカプセルも関節も別物になる。
//
// WHY «組めなかった» を再試行の理由に入れないか: 根ボーン名が骨格に無いときは何度
//     やっても組めない。毎フレーム全ノードのバインドポーズを組み直すだけになるので、
//     条件が変わるまで結果を据え置き、理由は RagdollStatus で名指しする。
bool NeedsRebuild(const RagdollComponent& ragdoll, const asset::Skeleton& skeleton)
{
    const RagdollRuntime& runtime = ragdoll.runtime;
    return !runtime.rig
        || runtime.builtSkeleton != &skeleton
        || runtime.builtRoot     != ragdoll.rootBoneName
        || runtime.builtMaxDepth != ragdoll.maxDepth
        || runtime.builtProfile  != ragdoll.profile;
}

// 衝撃で抜けた力みを戻す。
void AdvanceRecovery(RagdollComponent& ragdoll, float dt)
{
    if (ragdoll.mode != RagdollMode::Active || ragdoll.recoveryRemaining <= 0.0f) {
        ragdoll.recoveryRemaining = 0.0f;
        ragdoll.muscleScale       = 1.0f;
        return;
    }

    ragdoll.recoveryRemaining -= dt;
    if (ragdoll.recoveryRemaining <= 0.0f) {
        ragdoll.recoveryRemaining = 0.0f;
        ragdoll.muscleScale       = 1.0f;
        return;
    }

    const float total = std::max(ragdoll.recoverySeconds, math::EPSILON);
    ragdoll.muscleScale = math::Lerp(1.0f, 1.0f - math::Clamp01(ragdoll.impactSlack),
                                     math::Clamp01(ragdoll.recoveryRemaining / total));
}

// 自分の当たり判定を集める。ラグドール中も «立っている側» のコライダーは World に
// 残っているので、除外しないと倒れた体が自分自身と押し合う。
void CollectOwnColliders(GameObject& object, std::vector<const physics::Collider*>& out)
{
    const auto push = [&out](const ColliderComponent* component) {
        if (component && component->collider) out.push_back(component->collider.get());
    };
    push(object.GetComponent<AabbColliderComponent>());
    push(object.GetComponent<BoxColliderComponent>());
    push(object.GetComponent<SphereColliderComponent>());
    push(object.GetComponent<CapsuleColliderComponent>());
    push(object.GetComponent<CylinderColliderComponent>());
    push(object.GetComponent<MeshColliderComponent>());
    push(object.GetComponent<ConvexHullColliderComponent>());
    push(object.GetComponent<TerrainColliderComponent>());

    for (int i = 0, count = object.GetChildCount(); i < count; ++i)
        if (GameObject* child = object.GetChild(i)) CollectOwnColliders(*child, out);
}

// 接地面。アニメーション姿勢は常に «床を破っていない» 側に居なければならない ─
// 床が目標より上にあると、その下の剛体が毎ステップ押し上げられてサーボと押し合い、
// 足だけが震える。だからオーナーの足元と、今の姿勢の最下点の低い方を採る。
float GroundHeightFor(const RagdollComponent& ragdoll, const GameObject& owner)
{
    const float ownerFloor = owner.transform.worldPosition.y + ragdoll.groundOffset;
    return std::min(ownerFloor,
                    ragdoll.runtime.rig->LowestContactHeight(ragdoll.runtime.bones));
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

// 骨 GameObject の Transform を書き、smr を読む。宣言から漏らすと、それらを触る他の
// System と同じバッチに入って並列に走る (AnimatorSystem::GetAccess の WHY を参照)。
ComponentAccess RagdollSystem::GetAccess() const
{
    return ComponentAccess{}
        .Reads<SkinnedMeshRenderer>()
        .Writes<RagdollComponent, AnimatorComponent, BoneComponent, Transform>();
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

    std::vector<math::Vector3>    solvedPositions;
    std::vector<math::Quaternion> solvedRotations;

    for (EntityID id : entities) {
        GameObject* go = scene.GetGameObject(id);
        if (!go || !go->activeInHierarchy()) continue;

        auto* ragdoll = go->GetComponent<RagdollComponent>();
        if (!ragdoll) continue;

        if (!ragdoll->enabled || (!ctx.playing && !ragdoll->simulateInEditor)) {
            ragdoll->runtimeStatus = ragdoll->enabled
                ? RagdollStatus::NotPlaying
                : RagdollStatus::Disabled;
            ragdoll->runtimeBodyCount  = 0;
            ragdoll->runtimeJointCount = 0;
            ragdoll->runtimeSaturated  = 0;
            ragdoll->runtimeLimited    = 0;
            ragdoll->runtimeContacts   = 0;
            ragdoll->phase  = RagdollPhase::Idle;
            ragdoll->mode   = RagdollMode::Passive;
            ragdoll->weight = 0.0f;
            ragdoll->pendingImpulses.clear();
            ragdoll->beginRequested  = false;
            ragdoll->endRequested    = false;
            ragdoll->activeRequested = false;
            ragdoll->startTriggered  = false;
            continue;
        }

        if (ragdoll->phase == RagdollPhase::Idle && ragdoll->activateOnStart &&
            !ragdoll->startTriggered && !ragdoll->beginRequested) {
            ragdoll->startTriggered    = true;
            ragdoll->beginRequested    = true;
            ragdoll->activeRequested   = true;
            ragdoll->holdRemaining     = 0.0f;
            ragdoll->activationWeight  = 1.0f;
            ragdoll->activationGravity = 1.0f;
        }

        // 止まっているあいだは骨を 1 本も触らない。Animator が書いた姿勢がそのまま残る。
        if (ragdoll->phase == RagdollPhase::Idle && !ragdoll->beginRequested) {
            ragdoll->runtimeStatus    = RagdollStatus::Idle;
            ragdoll->runtimeDeviation = 0.0f;
            ragdoll->runtimeSaturated = 0;
            ragdoll->runtimeLimited   = 0;
            ragdoll->runtimeContacts  = 0;
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
        RagdollRuntime& runtime = ragdoll->runtime;

        if (NeedsRebuild(*ragdoll, skeleton)) {
            const RagdollProfile profile = ragdoll->ResolveProfile();
            BuildBoneList(*ragdoll, skeleton, *smr, profile);
            runtime.rig = std::make_unique<RagdollRig>();
            if (!runtime.bones.empty()) {
                runtime.rig->Build(
                    BuildBindPose(runtime, skeleton, go->transform.GetWorldMatrix()), profile);
            }
            runtime.builtSkeleton = &skeleton;
            runtime.builtRoot     = ragdoll->rootBoneName;
            runtime.builtMaxDepth = ragdoll->maxDepth;
            runtime.builtProfile  = ragdoll->profile;

            // «剛体は組めたが可動域が全部 fallback» は画面では «なんとなく柔らかい»
            // としか見えない。組み直したときだけ出るので、ログが溢れることもない。
            ragdoll->runtimeUnmatched =
                static_cast<int>(runtime.rig->UnmatchedBones().size());
            if (ragdoll->runtimeUnmatched > 0) {
                std::string names;
                for (const std::string& bone : runtime.rig->UnmatchedBones()) {
                    if (!names.empty()) names += ", ";
                    names += bone;
                }
                FBZZ_LOG_WARN("RagdollSystem: '%s' のプロファイルが %d 本の骨に当たって"
                              "いません。可動域が fallback の球関節になります: %s",
                              go->name.c_str(), ragdoll->runtimeUnmatched, names.c_str());
            }
        }

        if (!runtime.rig->IsBuilt()) {
            ragdoll->runtimeStatus     = RagdollStatus::NoParticles;
            ragdoll->runtimeBodyCount  = 0;
            ragdoll->runtimeJointCount = 0;
            ragdoll->phase             = RagdollPhase::Idle;
            ragdoll->beginRequested    = false;
            continue;
        }

        if (!SampleFkPose(runtime, scene, *smr)) {
            ragdoll->runtimeStatus  = RagdollStatus::NoBones;
            ragdoll->beginRequested = false;
            continue;
        }

        RagdollRig& rig = *runtime.rig;

        if (ragdoll->beginRequested) {
            ragdoll->beginRequested = false;
            ragdoll->endRequested   = false;
            ragdoll->mode = ragdoll->activeRequested ? RagdollMode::Active
                                                     : RagdollMode::Passive;
            ragdoll->activeRequested   = false;
            ragdoll->muscleScale       = 1.0f;
            ragdoll->recoveryRemaining = 0.0f;
            ragdoll->phase             = RagdollPhase::BlendIn;
            ragdoll->phaseTimer        = 0.0f;
            ragdoll->weight            = 0.0f;
            rig.Capture(runtime.bones);
            rig.UpdateDriveTargets(runtime.bones);
            ragdoll->runtimeGround = GroundHeightFor(*ragdoll, *go);

            // 起動のたびに拾い直す。ラグドール中にコライダーが増減することは無いので、
            // 毎フレーム階層を辿る理由が無い (ボスは骨だけで 100 個の子を持つ)。
            std::vector<const physics::Collider*> ignored;
            CollectOwnColliders(*go, ignored);
            rig.SetIgnoredColliders(std::move(ignored));
            if (auto* rigidBody = go->GetComponent<RigidBodyComponent>())
                rig.SetIgnoredBody(rigidBody->rigidBody.get());
        }

        // Active は目標ごと今のクリップへ乗せ替える。歩いて足元が上下しても床は付いて回る。
        // Passive は起動時の床のまま ─ 崩れ落ちている途中で床を動かすと体が跳ねる。
        if (ragdoll->mode == RagdollMode::Active) {
            rig.UpdateDriveTargets(runtime.bones);
            ragdoll->runtimeGround = GroundHeightFor(*ragdoll, *go);
        }

        if (ragdoll->endRequested && ragdoll->phase != RagdollPhase::BlendOut) {
            ragdoll->endRequested = false;
            ragdoll->phase        = RagdollPhase::BlendOut;
            ragdoll->phaseTimer   = 0.0f;
        }

        const float frameDt = std::max(ctx.dt, 0.0f);
        AdvanceRecovery(*ragdoll, frameDt);

        // 押された瞬間だけ力みを抜く。抜かないと «硬い体が少しめり込んで即座に戻る» に
        // なり、当たった側から見て手応えが無い。抜けたぶんは recoverySeconds で戻る。
        if (!ragdoll->pendingImpulses.empty() && ragdoll->mode == RagdollMode::Active) {
            ragdoll->muscleScale = std::min(ragdoll->muscleScale,
                                            1.0f - math::Clamp01(ragdoll->impactSlack));
            ragdoll->recoveryRemaining = std::max(ragdoll->recoverySeconds, 0.0f);
        }

        rig.SetSubsteps(ragdoll->substeps);
        rig.SetDrag(ragdoll->linearDrag, ragdoll->angularDrag);
        if (ragdoll->groundPlane) rig.SetGround(ragdoll->runtimeGround, ragdoll->friction);
        else                      rig.DisableGround();
        rig.SetGravity({ 0.0f,
                         -std::max(ragdoll->gravity, 0.0f) *
                             std::max(ragdoll->activationGravity, 0.0f),
                         0.0f });
        rig.SetLimitLearning(ragdoll->learnLimits,
                             std::max(ragdoll->limitMargin, 0.0f) * math::PI / 180.0f);
        // 繋ぎ止めも力みに追従させる。押された瞬間だけ胴が動くのはこの倍率が落ちるため。
        rig.SetRootAnchor(std::max(ragdoll->rootAnchor, 0.0f) *
                              math::Clamp01(ragdoll->muscleScale),
                          std::max(ragdoll->rootAnchorSag, 0.0f),
                          std::max(ragdoll->rootAnchorTilt, 0.0f));
        rig.SetDrive(ragdoll->mode == RagdollMode::Active,
                     std::max(ragdoll->driveScale, 0.0f) * math::Clamp01(ragdoll->muscleScale),
                     math::Clamp01(ragdoll->driveFalloff),
                     std::max(ragdoll->driveDamping, 0.0f));

        // Active 中に世界と当てると、足がカプセルの半径ぶん浮いた所でサーボと釣り合う
        // (RagdollComponent::contactWhileActive の WHY)。自己衝突は姿勢がクリップの
        // 近くに居る限り新しく重ならないので、こちらは Active でも残す。
        const bool worldContacts =
            ragdoll->mode != RagdollMode::Active || ragdoll->contactWhileActive;

        RagdollContactSettings contacts;
        contacts.world    = ragdoll->contactWorld   && worldContacts;
        contacts.dynamic  = ragdoll->contactDynamic && worldContacts;
        contacts.self     = ragdoll->contactSelf;
        contacts.selfSkip = ragdoll->selfSkip;
        contacts.surface.staticFriction  = ragdoll->friction;
        contacts.surface.dynamicFriction = ragdoll->friction;
        contacts.surface.restitution     = ragdoll->restitution;
        rig.SetContactSettings(contacts);

        for (const RagdollImpulse& impulse : ragdoll->pendingImpulses)
            rig.ApplyImpulse(impulse.origin, impulse.velocity, impulse.radius);
        ragdoll->pendingImpulses.clear();

        // 接触はフレームに 1 回作り、substep のあいだは使い回す。PhysX / Unity と同じ
        // 作りで、substep ごとに作り直すとナローフェーズが刻み数だけ走ることになる。
        rig.RefreshContacts(&ctx.world);
        rig.Step(std::min(frameDt, kMaxFrameStep));
        // 動かさずに «壁» として解いた相手へ、受けた反作用を 1 回だけ返す。
        rig.ApplyContactReactions();

        ragdoll->runtimeDeviation = rig.MeasureDeviation(runtime.bones);

        // 支え切れなくなったらサーボを捨てる。目標はこのフレームの姿勢のままなので、
        // 崩れ始めは «こらえていた形» から続く ─ Begin し直すと押された勢いが消える。
        if (ragdoll->mode == RagdollMode::Active && ragdoll->collapseDistance > 0.0f &&
            ragdoll->runtimeDeviation > ragdoll->collapseDistance) {
            ragdoll->mode              = RagdollMode::Passive;
            ragdoll->muscleScale       = 1.0f;
            ragdoll->recoveryRemaining = 0.0f;
        }

        rig.WritePose(runtime.bones, solvedPositions, solvedRotations);

        const float weight = AdvancePhase(*ragdoll, frameDt);
        ragdoll->runtimeBodyCount  = rig.GetBodyCount();
        ragdoll->runtimeJointCount = rig.GetJointCount();
        ragdoll->runtimeSaturated  = rig.CountSaturatedJoints();
        ragdoll->runtimeLimited    = rig.CountLimitedJoints();
        ragdoll->runtimeContacts   = rig.GetContactCount();
        ragdoll->runtimeStatus     = RagdollStatus::Running;
        if (weight <= 0.0f) continue;

        const math::Matrix4 ownerInv =
            math::Matrix4::Inverse(go->transform.GetWorldMatrix());

        for (std::size_t i = 0; i < runtime.bones.size(); ++i) {
            const math::Vector3 position =
                math::Vector3::Lerp(runtime.bones[i].position, solvedPositions[i], weight);
            const math::Quaternion rotation =
                math::Quaternion::Slerp(runtime.bones[i].rotation, solvedRotations[i], weight);
            CommitBoneWorldPose(scene, skeleton, *smr, *animator, ownerInv,
                                runtime.boneNodes[i], position, rotation,
                                runtime.boneScales[i]);
        }

        if (animator->skinningBuffer.IsValid())
            UploadBoneMatrices(*animator, resources);
    }
}

} // namespace fbzz::scene
