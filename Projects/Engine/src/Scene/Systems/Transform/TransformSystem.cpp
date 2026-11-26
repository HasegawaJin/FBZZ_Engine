/// @file    TransformSystem.cpp
/// @brief   親子階層のワールド Transform 更新。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note GameObject の親子関係は循環しないことを前提にする。
#include "Engine/Scene/Systems/Transform/TransformSystem.hpp"
#include "Engine/Core/Scheduler/SystemContext.hpp"
#include "Engine/Scene/Components/Physics/RigidBodyComponent.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Profiler/ProfileScope.hpp"
#include "Engine/Scene/Systems/Gameplay/GameplayComponentSystems.hpp"
#include <cmath>
#include <vector>
#include "WorldPoseHelpers.hpp"
#include "TransformWorklist.hpp"

namespace fbzz::scene {

static math::Vector3 ComputeWorldScale(const Transform& local, const Transform* parent)
{
    if (!parent) {
        return local.scale;
    }
    return {
        parent->worldScale.x * local.scale.x,
        parent->worldScale.y * local.scale.y,
        parent->worldScale.z * local.scale.z
    };
}

void UpdateWorldTransform(Transform& tf, const Transform* parentTransform) {
    if (parentTransform) {
        // 子の位置は親から見た距離。親の各軸の倍率を掛け、親の向きに回してから、世界空間での親の位置を足す。
        const math::Vector3 scaledLocal = {
            tf.position.x * parentTransform->worldScale.x,
            tf.position.y * parentTransform->worldScale.y,
            tf.position.z * parentTransform->worldScale.z
        };
        tf.worldRotation   = (parentTransform->worldRotation * tf.rotation).Normalized();
        tf.worldPosition   = parentTransform->worldPosition + parentTransform->worldRotation * scaledLocal;
    } else {
        tf.worldRotation   = tf.rotation;
        tf.worldPosition   = tf.position;
    }
    tf.worldScale = ComputeWorldScale(tf, parentTransform);
}

static void UpdateWorldTransform(GameObject& go, const Transform* parentTransform)
{
    // AI サービスで検証済みの演算だけをローカル姿勢と親行列のビット列で再利用し、走査と結果の公開は即時に行う。
    if (!world_pose_detail::TryReuseAiWorldMath(go.GetID().index, go.transform, parentTransform)) {
        UpdateWorldTransform(go.transform, parentTransform);
    }
}

namespace {

/// @note 同期中に callback は呼ばない。スレッドごとに容量だけを再利用し、呼び出し後に非所有参照を残さない。
struct TransformTraversalWorkspace {
    std::vector<GameObject*> pending;
    std::vector<GameObject*> ancestors;
    transform_detail::Worklist sceneWorklist;
};

thread_local TransformTraversalWorkspace s_transformTraversal;
}

static void AppendChildren(GameObject& object, std::vector<GameObject*>& pending)
{
    const int count = object.GetChildCount();
    for (int index = 0; index < count; ++index) {
        if (auto* child = object.GetChild(index)) {
            pending.push_back(child);
        }
    }
}

static void UpdateWorldTransformQueue(std::vector<GameObject*>& pending)
{
    for (size_t index = 0; index < pending.size(); ++index) {
        auto& object = *pending[index];
        // 起点の値コピーや破棄中の操作で、子一覧のリンクとシーン内の親が一致しない場合がある。
        const auto* parent = object.GetParent();
        UpdateWorldTransform(object, parent ? &parent->transform : nullptr);
        AppendChildren(object, pending);
    }
    pending.clear();
}

template<typename Update>
static void CollectAndPropagateScene(Scene& scene, const char* collectMarker, const char* propagateMarker, Update update)
{
    world_pose_detail::PrepareAiWorldMath();
    auto& worklist = s_transformTraversal.sceneWorklist;
    {
        const profiler::ProfileScope profile{profiler::ProfilerMarker(collectMarker)};
        worklist.Collect(scene);
    }
    {
        const profiler::ProfileScope profile{profiler::ProfilerMarker(propagateMarker)};
        worklist.Propagate(update);
    }
}

static void UpdateInterpolatedWorldTransform(GameObject& object, const Transform* parent, float alpha)
{
    const auto* body = object.GetComponent<RigidBodyComponent>();
    if (!body || !body->enabled || !body->rigidBody || body->rigidBody->IsStatic() ||
        !body->hasPhysicsPoseHistory) {
        UpdateWorldTransform(object, parent);
        return;
    }

    // 物理履歴はワールド座標なので位置と回転は直接補間し、子がこの描画用の姿勢を親として読む。
    Transform& pose = object.transform;
    const float inverseAlpha = 1.0f - alpha;
    pose.worldPosition = body->previousPhysicsPosition * inverseAlpha + body->lastPhysicsPosition * alpha;
    pose.worldRotation = math::Quaternion::Slerp(
        body->previousPhysicsRotation, body->lastPhysicsRotation, alpha).Normalized();
    pose.worldScale = ComputeWorldScale(pose, parent);
}

static void ApplyPhysicsInterpolation(Scene& scene, float alpha)
{
    FBZZ_PROFILE_SCOPE("Transform::InterpolateScene");
    // 物理の確定姿勢を変更しないため、描画補間は world だけへ適用する。
    CollectAndPropagateScene(scene, "Transform::CollectInterpolationRoots", "Transform::PropagateInterpolation",
        [alpha](GameObject& object, const Transform* parent) {
            UpdateInterpolatedWorldTransform(object, parent, alpha);
        });
}

ComponentAccess TransformSystem::GetAccess() const
{
    return ComponentAccess{}.Unrestricted();
}

static void InitializePhysicsSyncStates(Scene& scene)
{
    scene.ForEachComponentChunk<RigidBodyComponent>([&scene](auto ids, auto bodies) {
        for (std::size_t index = 0; index < ids.size(); ++index) {
            auto& body = bodies[index];
            if (!body.enabled || !body.rigidBody || body.hasPhysicsSyncState) {
                continue;
            }
            GameObject* object = scene.GetGameObject(ids[index]);
            if (!object) {
                continue;
            }
            const Transform& pose = object->transform;
            body.rigidBody->SetPosition(pose.worldPosition);
            body.rigidBody->SetRotation(pose.worldRotation);
            body.ResetPhysicsSyncState(pose.worldPosition, pose.worldRotation);
        }
    });
}

void TransformSystem::Update(SystemContext& ctx)
{
    Scene& scene = ctx.scene;
    if (ctx.simulating && GetPhase() == Phase::LateUpdate) {
        ApplyPhysicsInterpolation(scene, ctx.interpolationAlpha);
    }
    else {
        FlushWorldTransforms(scene);
    }

    // 同期済みの world で剛体を初期化してから FixedScript に渡す。初回の FixedUpdate が設定した姿勢を初期化で上書きしない。
    if (ctx.simulating && GetPhase() == Phase::PrePhysics) {
        InitializePhysicsSyncStates(scene);
    }
}

void FlushWorldTransforms(Scene& scene)
{
    FBZZ_PROFILE_SCOPE("Transform::FlushScene");
    CollectAndPropagateScene(scene, "Transform::CollectRoots", "Transform::PropagateScene",
        [](GameObject& object, const Transform* parent) {
            UpdateWorldTransform(object, parent);
        });
}

void FlushWorldTransforms(GameObject& root)
{
    FBZZ_PROFILE_SCOPE("Transform::FlushSubtree");
    world_pose_detail::PrepareAiWorldMath();
    const auto ancestorsProfile = profiler::Profiler::BeginScope(
        profiler::ProfilerMarker("Transform::ResolveAncestors"));
    auto& ancestors = s_transformTraversal.ancestors;
    ancestors.clear();
    for (GameObject* parent = root.GetParent(); parent; parent = parent->GetParent()) {
        ancestors.push_back(parent);
    }
    // 親の world に以前の描画補間値が残り得るため、祖先を上から local で計算し直してから対象を更新する。
    /// @see Docs/design/script-transform-contract.md
    const Transform* parent = nullptr;
    for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
        GameObject& ancestor = **it;
        UpdateWorldTransform(ancestor, parent);
        parent = &ancestor.transform;
    }
    ancestors.clear();
    profiler::Profiler::EndScope(ancestorsProfile);
    FBZZ_PROFILE_SCOPE("Transform::PropagateSubtree");
    UpdateWorldTransform(root, parent);
    if (root.GetChildCount() == 0) {
        return;
    }
    auto& pending = s_transformTraversal.pending;
    pending.clear();
    AppendChildren(root, pending);
    UpdateWorldTransformQueue(pending);
}

void SetWorldPose(Transform& tf, const Transform* parent,
                  const math::Vector3& worldPosition,
                  const math::Quaternion& worldRotation,
                  const math::Vector3& worldScale)
{
    // 親のゼロスケール軸で除算せず、有限なローカル値へ倒す。
    const auto divideSafe = [](const math::Vector3& a, const math::Vector3& b) {
        return math::Vector3{
            std::abs(b.x) > 1e-6f ? a.x / b.x : 0.0f,
            std::abs(b.y) > 1e-6f ? a.y / b.y : 0.0f,
            std::abs(b.z) > 1e-6f ? a.z / b.z : 0.0f
        };
    };

    tf.worldPosition = worldPosition;
    tf.worldRotation = worldRotation.Normalized();
    tf.worldScale    = worldScale;

    if (parent) {
        const Transform& pt = *parent;
        const math::Quaternion inverseParentRotation = pt.worldRotation.Inverse();
        tf.position = divideSafe(
            inverseParentRotation * (worldPosition - pt.worldPosition), pt.worldScale);
        tf.rotation = (inverseParentRotation * worldRotation).Normalized();
        tf.scale    = divideSafe(worldScale, pt.worldScale);
    } else {
        tf.position = worldPosition;
        tf.rotation = worldRotation.Normalized();
        tf.scale    = worldScale;
    }
}

void SetWorldPose(GameObject& go,
                  const math::Vector3& worldPosition,
                  const math::Quaternion& worldRotation,
                  const math::Vector3& worldScale)
{
    const GameObject* parent = go.GetParent();
    SetWorldPose(go.transform, parent ? &parent->transform : nullptr,
        worldPosition, worldRotation, worldScale);
}

}
