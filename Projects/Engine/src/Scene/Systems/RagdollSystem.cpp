/// @file    RagdollSystem.cpp
/// @brief   スケルトンと XPBD 関節体の橋渡し。捕獲・駆動・ブレンド・書き戻し
/// @author  Hasegawa Jin
/// @date    2026-09-01
/// @note 物理は RagdollRig / XPBDSolver に任せ、骨格と剛体の姿勢変換だけを担う。
#include <Engine/Scene/Systems/RagdollSystem.hpp>
#include "BonePoseHelpers.hpp"

#include "Engine/Core/Scheduler/SystemContext.hpp"
#include "Engine/Scene/Systems/SpringBoneSystem.hpp"
#include <Engine/Asset/Skeleton.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Profiler/ProfileScope.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/BoneComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/RagdollComponent.hpp>
#include <Engine/Scene/Ragdoll/RagdollControls.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Ragdoll/RagdollPlayback.hpp>
#include <Engine/Scene/Ragdoll/RagdollPose.hpp>
#include <Engine/Scene/SkinnedPoseBounds.hpp>
#include <Engine/Scene/Scene.hpp>
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

int FindNode(const asset::Skeleton& skeleton, const std::string& boneName)
{
    if (boneName.empty()) return skeleton.rootNodeIndex;
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
    for (std::size_t i = 0; i < std::min(animator.boneMatrices.size(),
                                       static_cast<std::size_t>(asset::MAX_SKINNING_BONES)); ++i)
        cb.boneMatrices[i] = animator.boneMatrices[i];
    resources.Update(animator.skinningBuffer, &cb, sizeof(SkinningCB));
}

/// @note 行優先、平行移動は 4 列目。
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

/// @note RagdollRig が剛体を持たない骨を親から埋めるため、親が必ず子より先に来る順を保つ。
/// @param claimed 複数の根で同じ骨へ二重に剛体を作り、競合する姿勢を書き戻すことを防ぐ。
void AppendBoneTree(RagdollRuntime& runtime,
                    const asset::Skeleton& skeleton,
                    const RagdollProfile& profile,
                    int rootNode,
                    int maxDepth,
                    const std::vector<bool>& excluded,
                    std::vector<bool>& claimed)
{
    if (rootNode < 0 || rootNode >= static_cast<int>(skeleton.nodes.size())) return;
    if (claimed[static_cast<std::size_t>(rootNode)]) return;

    struct Pending { int node; int parent; int depth; };
    std::vector<Pending> queue;
    queue.push_back({ rootNode, -1, 0 });

    for (std::size_t head = 0; head < queue.size(); ++head) {
        const Pending current = queue[head];
        if (current.node < 0 || current.node >= static_cast<int>(skeleton.nodes.size()))
            continue;
        if (claimed[static_cast<std::size_t>(current.node)] || excluded[static_cast<std::size_t>(current.node)]) continue;
        claimed[static_cast<std::size_t>(current.node)] = true;

        RagdollBonePose bone;
        bone.parent = current.parent;
        bone.name   = bone_pose_detail::CanonicalBoneName(skeleton.nodes[static_cast<std::size_t>(current.node)].name);
        const int index = static_cast<int>(runtime.bones.size());
        runtime.bones.push_back(std::move(bone));
        runtime.boneNodes.push_back(current.node);

        const bool depthReached = maxDepth > 0 && current.depth + 1 >= maxDepth;
        if (depthReached) continue;

        for (int child : skeleton.nodes[static_cast<std::size_t>(current.node)].children) {
            if (child < 0 || child >= static_cast<int>(skeleton.nodes.size())) continue;
            /// @note 除外する枝は骨ごと切り、親のカプセルが指へ向かう短い節になることも防ぐ。
            if (profile.IsExcluded(
                    bone_pose_detail::CanonicalBoneName(skeleton.nodes[static_cast<std::size_t>(child)].name)))
                continue;
            queue.push_back({ child, index, current.depth + 1 });
        }
    }
}

/// @note RagdollRig は複数の根を個別に繋ぎ止めるため、全体を一つのリグで解き、接触探索も一度で済ませる。
void BuildBoneList(RagdollComponent& ragdoll,
                   const asset::Skeleton& skeleton,
                   const RagdollProfile& profile)
{
    RagdollRuntime& runtime = ragdoll.runtime;
    runtime.bones.clear();
    runtime.boneNodes.clear();
    runtime.boneScales.clear();

    std::vector<bool> claimed(skeleton.nodes.size(), false);
    std::vector<bool> excluded(skeleton.nodes.size(), false);
    for (const auto& name : ragdoll.excludedRootBones) {
        if (name.empty()) continue;
        const int node = FindNode(skeleton, name);
        if (node < 0 || node >= static_cast<int>(skeleton.nodes.size())) {
            FBZZ_LOG_WARN("RagdollSystem: 除外ボーン '%s' が見つかりません", name.c_str());
            continue;
        }
        std::vector<int> pending{ node };
        while (!pending.empty()) {
            const int current = pending.back();
            pending.pop_back();
            if (current < 0 || current >= static_cast<int>(skeleton.nodes.size())) continue;
            const auto index = static_cast<std::size_t>(current);
            if (excluded[index]) continue;
            excluded[index] = true;
            for (const int child : skeleton.nodes[index].children) pending.push_back(child);
        }
    }
    AppendBoneTree(runtime, skeleton, profile,
                   FindNode(skeleton, ragdoll.rootBoneName), ragdoll.maxDepth, excluded, claimed);
    for (const std::string& extra : ragdoll.extraRootBones) {
        if (extra.empty()) continue;
        AppendBoneTree(runtime, skeleton, profile,
                       FindNode(skeleton, extra), ragdoll.maxDepth, excluded, claimed);
    }

    runtime.boneScales.assign(runtime.bones.size(), math::Vector3::ONE);
}

/// @note スケルトン空間のバインド姿勢を親から子の順で確定する。
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

/// @note 関節可動域の基準が起動フレームに依存しないよう、バインドポーズで剛体と関節を組む。
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
        const auto matrix = toWorld * globals[static_cast<std::size_t>(node)];
        DecomposeAffine(matrix, pose[i].position, pose[i].rotation);
        pose[i].scale = {
            math::Vector3{matrix.m[0][0],matrix.m[1][0],matrix.m[2][0]}.Length(),
            math::Vector3{matrix.m[0][1],matrix.m[1][1],matrix.m[2][1]}.Length(),
            math::Vector3{matrix.m[0][2],matrix.m[1][2],matrix.m[2][2]}.Length()};
    }
    return pose;
}

/// @note このフレームの FK/IK/揺れ込みの姿勢が、捕獲元・サーボ目標・ブレンド先を兼ねる。
bool SampleFkPose(RagdollRuntime& runtime, Scene& scene, const SkinnedMeshRenderer& smr)
{
    for (std::size_t i = 0; i < runtime.bones.size(); ++i) {
        GameObject* bone = bone_pose_detail::BoneObject(scene, smr, runtime.boneNodes[i]);
        if (!bone) return false;
        runtime.bones[i].position = bone->transform.worldPosition;
        runtime.bones[i].rotation = bone->transform.worldRotation;
        runtime.bones[i].scale = bone->transform.worldScale;
        runtime.boneScales[i]     = bone->transform.worldScale;
    }
    return true;
}

/// @note 骨の並びが変わるとカプセルも関節も変わるため再構築する。
/// @note 構築失敗だけでは再試行せず、設定が変わるまで結果と RagdollStatus を保持する。
bool NeedsRebuild(const RagdollComponent& ragdoll, const asset::Skeleton& skeleton,
                  const math::Vector3& scale)
{
    const RagdollRuntime& runtime = ragdoll.runtime;
    return !runtime.rig
        || runtime.builtDefinitionRevision != runtime.definitionRevision
        || runtime.builtSkeleton   != &skeleton
        || runtime.builtNodeCount != skeleton.nodes.size()
        || (runtime.builtScale - scale).LengthSq() > 1.0e-8f
        || runtime.builtRoot       != ragdoll.rootBoneName
        || runtime.builtExtraRoots != ragdoll.extraRootBones
        || runtime.builtMaxDepth   != ragdoll.maxDepth
        || runtime.builtProfile    != ragdoll.profile
        || runtime.builtExcludedRoots != ragdoll.excludedRootBones;
}

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

/// @note 通常コライダーはラグドール中も World に残るため、自分自身との押し合いを除外する。
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

/// @note 床とサーボが押し合う振動を防ぐため、オーナーの足元と目標姿勢の最下点の低い方を採る。
float GroundHeightFor(const RagdollComponent& ragdoll, const GameObject& owner)
{
    const float ownerFloor = owner.transform.worldPosition.y + ragdoll.groundOffset;
    return std::min(ownerFloor,
                    ragdoll.runtime.rig->LowestContactHeight(ragdoll.runtime.bones));
}

}

/// @note 骨 Transform の書き込みと smr の読み取りを宣言し、骨姿勢システム間の並列実行を防ぐ。
/// @see AnimatorSystem::GetAccess
ComponentAccess RagdollSystem::GetAccess() const
{
    return ComponentAccess{}
        .Reads<SkinnedMeshRenderer>()
        .Writes<RagdollComponent, AnimatorComponent, BoneComponent, Transform>();
}

OrderingHints RagdollSystem::GetOrder() const
{
    /// @note 揺れものが確定させた姿勢がブレンド先になる。逆順だと戻り際に揺れが物理を打ち消す。
    return OrderingHints{}.After<SpringBoneSystem>();
}

void RagdollSystem::Update(SystemContext& ctx)
{
    FBZZ_PROFILE_SCOPE("RagdollSystem");

    Scene& scene = ctx.scene;

    const auto span     = scene.GetEntities<RagdollComponent>();
    const auto entities = std::vector<EntityID>(span.begin(), span.end());

    std::vector<math::Vector3>    solvedPositions;
    std::vector<math::Quaternion> solvedRotations;

    for (EntityID id : entities) {
        GameObject* go = scene.GetGameObject(id);
        if (!go) continue;

        auto* ragdoll = go->GetComponent<RagdollComponent>();
        if (!ragdoll) continue;
        if (!go->activeInHierarchy()) {
            if (ragdoll->runtime.rig) ragdoll->runtime.rig->Controls().ReleaseAll();
            continue;
        }

        if (!ragdoll->enabled || (!ctx.playing && !ragdoll->simulateInEditor)) {
            if (ragdoll->runtime.rig) ragdoll->runtime.rig->Controls().ReleaseAll();
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
            ragdoll->runtime.remainingTime = 0.0;
            continue;
        }

        /// @note Pause は最終姿勢・駆動履歴・未処理命令を保持し、再開後の固定更新で消費する。
        if (ctx.playing && !ctx.simulating) continue;

        if (ragdoll->phase == RagdollPhase::Idle && ragdoll->activateOnStart &&
            !ragdoll->startTriggered && !ragdoll->beginRequested) {
            ragdoll->beginRequested    = true;
            ragdoll->activeRequested   = true;
            ragdoll->holdRemaining     = 0.0f;
            ragdoll->activationWeight  = 1.0f;
            ragdoll->activationGravity = 1.0f;
        }

        /// @note 止まっているあいだは骨を 1 本も触らない。Animator が書いた姿勢がそのまま残る。
        if (ragdoll->phase == RagdollPhase::Idle && !ragdoll->beginRequested) {
            if (ragdoll->runtime.rig) ragdoll->runtime.rig->Controls().ReleaseAll();
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
            continue;
        }
        auto* smr = bone_pose_detail::FindSkinnedMeshRenderer(*go);
        if (!smr || !smr->model || !smr->model->skeleton) {
            ragdoll->runtimeStatus  = RagdollStatus::NoSkinnedMesh;
            continue;
        }

        const asset::Skeleton& skeleton = *smr->model->skeleton;
        RagdollRuntime& runtime = ragdoll->runtime;
        if (animator->nodeGlobalTransforms.size() != skeleton.nodes.size()) {
            ragdoll->runtimeStatus = RagdollStatus::NoBones;
            continue;
        }

        const int assetGeneration = asset::AssetManager::GetAssetGeneration();
        if (runtime.loadedReference != ragdoll->physicsAssetPath || runtime.loadedAssetGeneration != assetGeneration) {
            runtime.loadedReference = ragdoll->physicsAssetPath;
            runtime.loadedAssetGeneration = assetGeneration;
            if (ragdoll->physicsAssetPath.empty()) {
                if (runtime.definitionValid) ++runtime.definitionRevision;
                runtime.definitionValid = false;
            } else {
                RagdollDefinition candidate;
                std::string error;
                if (LoadRagdollDefinition(ragdoll->physicsAssetPath,candidate,&error)) {
                    if (!runtime.definitionValid || candidate.signature != runtime.definition.signature) {
                        runtime.definition = std::move(candidate);
                        runtime.definitionValid = true;
                        ++runtime.definitionRevision;
                    }
                } else FBZZ_LOG_WARN("Ragdoll physics asset: %s",error.c_str());
            }
        }
        if (!ragdoll->physicsAssetPath.empty() && !runtime.definitionValid) {
            ragdoll->runtimeStatus = RagdollStatus::InvalidPhysicsAsset;
            continue;
        }
        /// @note 実行中の再構築後は現在姿勢を捕獲し、垂れた部分木がバインドポーズへ跳ね戻ることを防ぐ。
        const bool rebuilding = NeedsRebuild(*ragdoll, skeleton, go->transform.worldScale);
        if (rebuilding) {
            RagdollProfile profile = ragdoll->ResolveProfile();
            if (runtime.definitionValid) ApplyRagdollDefinition(runtime.definition,profile);
            BuildBoneList(*ragdoll, skeleton, profile);
            runtime.rig = std::make_unique<RagdollRig>();
            if (!runtime.bones.empty()) {
                runtime.rig->Build(
                    BuildBindPose(runtime, skeleton, go->transform.GetWorldMatrix()), profile);
            }
            for (const auto& overrideDrive:runtime.driveOverrides)
                (void)runtime.rig->SetBodyDrive(overrideDrive.bone,overrideDrive.settings);
            runtime.builtDefinitionRevision = runtime.definitionRevision;
            runtime.builtSkeleton   = &skeleton;
            runtime.builtNodeCount = skeleton.nodes.size();
            runtime.builtScale = go->transform.worldScale;
            runtime.builtRoot       = ragdoll->rootBoneName;
            runtime.builtExtraRoots = ragdoll->extraRootBones;
            runtime.builtMaxDepth   = ragdoll->maxDepth;
            runtime.builtProfile    = ragdoll->profile;
            runtime.builtExcludedRoots = ragdoll->excludedRootBones;

            /// @note 可動域の fallback は見た目で判別しにくいため、再構築時だけ診断を出す。
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
            continue;
        }

        if (!SampleFkPose(runtime, scene, *smr)) {
            ragdoll->runtimeStatus  = RagdollStatus::NoBones;
            continue;
        }

        RagdollRig& rig = *runtime.rig;
        const RagdollOwnerPose ownerPose{go->transform.worldPosition,go->transform.worldRotation};
        bool capturedTargets = false;

        /// @note 再構築で全体が再ブレンドしないよう、現在姿勢を捕獲して phase と重みは保つ。
        if (rebuilding && ragdoll->phase != RagdollPhase::Idle && !ragdoll->beginRequested) {
            rig.Capture(runtime.bones);
            capturedTargets = true;
            rig.UpdateDriveTargets(runtime.bones,0.0f,&ownerPose);
            std::vector<const physics::Collider*> ignored;
            CollectOwnColliders(*go, ignored);
            rig.SetIgnoredColliders(std::move(ignored));
            if (auto* rigidBody = go->GetComponent<RigidBodyComponent>())
                rig.SetIgnoredBody(rigidBody->rigidBody.get());
        }

        if (ragdoll->beginRequested) {
            const bool needsCapture = rebuilding || ragdoll->phase == RagdollPhase::Idle;
            ragdoll->phaseStartWeight = ragdoll->phase == RagdollPhase::Idle
                ? 0.0f : math::Clamp01(ragdoll->weight);
            ragdoll->startTriggered = true;
            runtime.remainingTime = 0.0;
            ragdoll->beginRequested = false;
            ragdoll->endRequested   = false;
            ragdoll->mode = ragdoll->activeRequested ? RagdollMode::Active
                                                     : RagdollMode::Passive;
            ragdoll->activeRequested   = false;
            ragdoll->muscleScale       = 1.0f;
            ragdoll->recoveryRemaining = 0.0f;
            ragdoll->phase             = RagdollPhase::BlendIn;
            ragdoll->phaseTimer        = 0.0f;
            ragdoll->weight            = ragdoll->phaseStartWeight;
            if (needsCapture) {
                rig.Capture(runtime.bones);
                capturedTargets = true;
            }
            rig.UpdateDriveTargets(runtime.bones,0.0f,&ownerPose);
            ragdoll->runtimeGround = GroundHeightFor(*ragdoll, *go);

            /// @note 実行中はコライダーが増減しないため、起動時だけ階層を辿って拾い直す。
            std::vector<const physics::Collider*> ignored;
            CollectOwnColliders(*go, ignored);
            rig.SetIgnoredColliders(std::move(ignored));
            if (auto* rigidBody = go->GetComponent<RigidBodyComponent>())
                rig.SetIgnoredBody(rigidBody->rigidBody.get());
        }

        /// @note Active の床は現在の目標に追従し、Passive では崩れる途中の跳ねを防ぐため起動時の床を保つ。
        rig.SetStandingGuard(ragdoll->standingGuard, ragdoll->standingMaxDistance,
                             ragdoll->standingMaxDegrees * math::PI / 180.0f);
        if (ragdoll->mode == RagdollMode::Active || ragdoll->standingGuard) {
            /// @note 再捕獲で目標時計を作り直しても、未消費の固定刻みの端数は捨てない。
            const float sampleDt = ctx.dt + (capturedTargets ? static_cast<float>(runtime.remainingTime) : 0.0f);
            rig.UpdateDriveTargets(runtime.bones,sampleDt,&ownerPose);
            ragdoll->runtimeGround = GroundHeightFor(*ragdoll, *go);
        }

        if (ragdoll->endRequested && ragdoll->phase != RagdollPhase::BlendOut) {
            BeginRagdollBlendOut(*ragdoll);
        }

        const float elapsed = (ctx.simulating || (!ctx.playing && ragdoll->simulateInEditor))
            ? ctx.dt : 0.0f;
        const int steps = elapsed > 0.0f ? AccumulateRagdollSteps(runtime.remainingTime, elapsed) : 0;

        /// @note 即座に戻る硬い反応を避けるため、衝撃時だけ力みを抜き recoverySeconds で戻す。
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
        /// @note 繋ぎ止めも力みに追従させる。押された瞬間だけ胴が動くのはこの倍率が落ちるため。
        rig.SetRootAnchor(std::max(ragdoll->rootAnchor, 0.0f) *
                              math::Clamp01(ragdoll->muscleScale),
                          std::max(ragdoll->rootAnchorSag, 0.0f),
                          std::max(ragdoll->rootAnchorTilt, 0.0f));
        rig.SetDrive(ragdoll->mode == RagdollMode::Active,
                     std::max(ragdoll->driveScale, 0.0f) * math::Clamp01(ragdoll->muscleScale),
                     math::Clamp01(ragdoll->driveFalloff),
                     std::max(ragdoll->driveDamping, 0.0f));

        /// @note Active の環境接触は足の浮きを招くため設定に従い、クリップ近傍で新しい重なりがない自己衝突は保つ。
        /// @see RagdollComponent::contactWhileActive
        const bool worldContacts =
            ragdoll->mode != RagdollMode::Active || ragdoll->contactWhileActive;

        RagdollContactSettings contacts;
        contacts.world    = ragdoll->contactWorld   && worldContacts;
        contacts.dynamic  = ragdoll->contactDynamic && worldContacts;
        contacts.self     = ragdoll->contactSelf;
        contacts.selfSkip = ragdoll->selfSkip;
        contacts.allowRest = ragdoll->allowRest;
        contacts.surface.staticFriction  = ragdoll->staticFriction < 0.0f ? ragdoll->friction : ragdoll->staticFriction;
        contacts.surface.dynamicFriction = ragdoll->friction;
        contacts.surface.restitution     = ragdoll->restitution;
        rig.SetContactSettings(contacts);

        for (const RagdollImpulse& impulse : ragdoll->pendingImpulses) {
            rig.ApplyImpulse(impulse.origin, impulse.velocity, impulse.radius);
            rig.ApplyAngularVelocity(impulse.origin, impulse.angularVelocity, impulse.radius);
        }
        ragdoll->pendingImpulses.clear();

        /// @note 世界への参照を固定刻みごとに渡し、接触は Rig が各 substep の積分後に更新する。
        for (int step = 0; step < steps; ++step) {
            if (ragdoll->endRequested && ragdoll->phase != RagdollPhase::BlendOut)
                BeginRagdollBlendOut(*ragdoll);
            AdvanceRecovery(*ragdoll, RAGDOLL_FIXED_STEP);
            rig.SetRootAnchor(std::max(ragdoll->rootAnchor, 0.0f) * math::Clamp01(ragdoll->muscleScale),
                              std::max(ragdoll->rootAnchorSag, 0.0f), std::max(ragdoll->rootAnchorTilt, 0.0f));
            rig.SetDrive(ragdoll->mode == RagdollMode::Active,
                         std::max(ragdoll->driveScale, 0.0f) * math::Clamp01(ragdoll->muscleScale),
                         math::Clamp01(ragdoll->driveFalloff), std::max(ragdoll->driveDamping, 0.0f));
            rig.RefreshContacts(&ctx.world);
            rig.Step(RAGDOLL_FIXED_STEP);
            rig.ApplyContactReactions();
            AdvanceRagdollPhase(*ragdoll, RAGDOLL_FIXED_STEP);
            if (ragdoll->phase == RagdollPhase::Idle) {
                rig.Controls().ReleaseAll();
                runtime.remainingTime = 0.0;
                break;
            }
        }

        ragdoll->runtimeDeviation = rig.MeasureDeviation(runtime.bones);

        /// @note 支え切れないときは現在姿勢から崩し、再 Begin で衝撃を失わないようサーボだけを捨てる。
        if (ragdoll->mode == RagdollMode::Active && ragdoll->collapseDistance > 0.0f &&
            ragdoll->runtimeDeviation > ragdoll->collapseDistance) {
            ragdoll->mode              = RagdollMode::Passive;
            ragdoll->muscleScale       = 1.0f;
            ragdoll->recoveryRemaining = 0.0f;
        }

        rig.WritePose(runtime.bones, solvedPositions, solvedRotations);

        const float weight = math::Clamp01(ragdoll->weight);
        ragdoll->runtimeBodyCount  = rig.GetBodyCount();
        ragdoll->runtimeJointCount = rig.GetJointCount();
        ragdoll->runtimeSaturated  = rig.CountSaturatedJoints();
        ragdoll->runtimeLimited    = rig.CountLimitedJoints();
        ragdoll->runtimeContacts   = rig.GetContactCount();
        ragdoll->runtimeStatus     = RagdollStatus::Running;
        if (weight <= 0.0f) continue;

        const math::Matrix4 ownerInv =
            math::Matrix4::Inverse(go->transform.GetWorldMatrix());
        const auto originalGlobals = animator->nodeGlobalTransforms;
        std::vector<bool> selected(skeleton.nodes.size(), false);
        std::vector<math::Vector3> scales(skeleton.nodes.size(), math::Vector3::ONE);
        for (std::size_t i = 0; i < scales.size(); ++i)
            if (const auto* bone = bone_pose_detail::BoneObject(scene, *smr, static_cast<int>(i)))
                scales[i] = bone->transform.worldScale;
        BlendRagdollPose(runtime.bones, weight, ragdoll->standingGuard, solvedPositions, solvedRotations);
        const auto rootTransform = math::Matrix4::Inverse(skeleton.rootInverseTransform);
        for (std::size_t i = 0; i < runtime.bones.size(); ++i) {
            const auto node = static_cast<std::size_t>(runtime.boneNodes[i]);
            selected[node] = true;
            animator->nodeGlobalTransforms[node] = rootTransform * ownerInv *
                math::Matrix4::TRS(solvedPositions[i], solvedRotations[i], runtime.boneScales[i]);
        }
        PropagateRagdollDescendants(skeleton, originalGlobals, selected, animator->nodeGlobalTransforms);
        const auto toWorld = go->transform.GetWorldMatrix() * skeleton.rootInverseTransform;
        std::vector<int> pending;
        for (std::size_t i = 0; i < skeleton.nodes.size(); ++i)
            if (skeleton.nodes[i].parentIndex < 0) pending.push_back(static_cast<int>(i));
        std::vector<bool> visited(skeleton.nodes.size(), false);
        while (!pending.empty()) {
            const int node = pending.back();
            pending.pop_back();
            if (node < 0 || node >= static_cast<int>(skeleton.nodes.size())) continue;
            const auto n = static_cast<std::size_t>(node);
            if (visited[n]) continue;
            visited[n] = true;
            math::Vector3 position;
            math::Quaternion rotation;
            DecomposeAffine(toWorld * animator->nodeGlobalTransforms[n], position, rotation);
            bone_pose_detail::CommitBoneWorldPose(scene, skeleton, *smr, *animator, ownerInv,
                                node, position, rotation, scales[n]);
            for (const int child : skeleton.nodes[n].children) pending.push_back(child);
        }
        UpdateSkinnedPoseBounds(*animator, skeleton);

        if (ctx.resources && animator->skinningBuffer.IsValid())
            UploadBoneMatrices(*animator, *ctx.resources);
    }
}

}
