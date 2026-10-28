/// @file    TransformSystem.cpp
/// @brief   親子階層のワールド Transform 更新。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note GameObject の親子関係は循環しないことを前提にする。
#include "Engine/Scene/Systems/TransformSystem.hpp"
#include "Engine/Core/Scheduler/SystemContext.hpp"
#include "Engine/Scene/Components/RigidBodyComponent.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Profiler/ProfileScope.hpp"
#include "Engine/Scene/Systems/GameplayComponentSystems.hpp"
#include <cmath>
#include <vector>
#include "WorldPoseHelpers.hpp"
#include "TransformWorklist.hpp"

namespace fbzz::scene {

void UpdateWorldTransform(Transform& tf, const Transform* parentTransform) {
    if (parentTransform) {
        math::Vector3 scaledLocal = {
            tf.position.x * parentTransform->worldScale.x,
            tf.position.y * parentTransform->worldScale.y,
            tf.position.z * parentTransform->worldScale.z
        };
        tf.worldRotation   = (parentTransform->worldRotation * tf.rotation).Normalized();
        tf.worldPosition   = parentTransform->worldPosition + parentTransform->worldRotation * scaledLocal;
        tf.worldScale = {
            parentTransform->worldScale.x * tf.scale.x,
            parentTransform->worldScale.y * tf.scale.y,
            parentTransform->worldScale.z * tf.scale.z
        };
    } else {
        tf.worldRotation   = tf.rotation;
        tf.worldPosition   = tf.position;
        tf.worldScale = tf.scale;
    }
}

static void UpdateWorldTransform(GameObject& go, const Transform* parentTransform)
{
    /// @note Reuse only arithmetic already validated by the AI service, with bitwise local/parent keys; traversal and publication remain immediate.
    if (!world_pose_detail::TryReuseAiWorldMath(go.GetID().index, go.transform, parentTransform))
        UpdateWorldTransform(go.transform, parentTransform);
}

namespace {
/// @note 同期中に callback は呼ばない。スレッドごとに容量だけを再利用し、呼び出し後に非所有参照を残さない。
struct TransformTraversalWorkspace {
    std::vector<GameObject*> pending;
    std::vector<GameObject*> ancestors;
    transform_detail::Worklist sceneWorklist;
};

thread_local TransformTraversalWorkspace s_transformTraversal;
} /// @note namespace

static void UpdateWorldTransformQueue(std::vector<GameObject*>& queue)
{
    for (size_t index = 0; index < queue.size(); ++index) {
        GameObject* go = queue[index];
        const GameObject* parent = go->GetParent();
        UpdateWorldTransform(*go, parent ? &parent->transform : nullptr);
        for (int i = 0; i < go->GetChildCount(); ++i)
            if (auto* child = go->GetChild(i)) queue.push_back(child);
    }
    queue.clear();
}

static void ApplyPhysicsInterpolation(Scene& scene, float alpha)
{
    FBZZ_PROFILE_SCOPE("Transform::InterpolateScene");
    /// @note 物理の確定姿勢を変更しないため、描画補間は world だけへ適用する。
    world_pose_detail::PrepareAiWorldMath();
    auto& worklist = s_transformTraversal.sceneWorklist;
    const auto rootsProfile = profiler::Profiler::BeginScope(
        profiler::ProfilerMarker("Transform::CollectInterpolationRoots"));
    worklist.Collect(scene);
    profiler::Profiler::EndScope(rootsProfile);
    const auto propagationProfile = profiler::Profiler::BeginScope(
        profiler::ProfilerMarker("Transform::PropagateInterpolation"));
    worklist.Propagate([alpha](GameObject& object, const Transform* parentTf) {
        GameObject* go = &object;
        auto* rb = go->GetComponent<RigidBodyComponent>();
        if (rb && rb->enabled && rb->rigidBody && !rb->rigidBody->IsStatic() &&
            rb->hasPhysicsPoseHistory) {
            const float inverseAlpha = 1.0f - alpha;
            go->transform.worldPosition =
                rb->previousPhysicsPosition * inverseAlpha +
                rb->lastPhysicsPosition * alpha;
            go->transform.worldRotation = math::Quaternion::Slerp(
                rb->previousPhysicsRotation,
                rb->lastPhysicsRotation,
                alpha).Normalized();

            if (parentTf) {
                go->transform.worldScale = {
                    parentTf->worldScale.x * go->transform.scale.x,
                    parentTf->worldScale.y * go->transform.scale.y,
                    parentTf->worldScale.z * go->transform.scale.z
                };
            } else {
                go->transform.worldScale = go->transform.scale;
            }
        } else {
            UpdateWorldTransform(*go, parentTf);
        }

    });
    profiler::Profiler::EndScope(propagationProfile);
}

ComponentAccess TransformSystem::GetAccess() const
{
    return ComponentAccess{}.Unrestricted();
}

void TransformSystem::Update(SystemContext& ctx)
{
    Scene& scene = ctx.scene;
    if (ctx.simulating && GetPhase() == Phase::LateUpdate)
        ApplyPhysicsInterpolation(scene, ctx.interpolationAlpha);
    else
        FlushWorldTransforms(scene);

    if (ctx.simulating && GetPhase() == Phase::PrePhysics) {
        /// @note 最初の OnFixedUpdate が書いた姿勢を上書きしないよう、物理履歴は FixedScript より前に初期化する。
        scene.ForEachComponentChunk<RigidBodyComponent>([&scene](auto ids, auto bodies) {
            for (std::size_t index = 0; index < ids.size(); ++index) {
                auto& body = bodies[index];
                if (!body.enabled || !body.rigidBody || body.hasPhysicsSyncState)
                    continue;
                GameObject* go = scene.GetGameObject(ids[index]);
                if (!go) continue;
                body.rigidBody->SetPosition(go->transform.worldPosition);
                body.rigidBody->SetRotation(go->transform.worldRotation);
                body.ResetPhysicsSyncState(
                    go->transform.worldPosition, go->transform.worldRotation);
            }
        });
    }
}

void FlushWorldTransforms(Scene& scene)
{
    FBZZ_PROFILE_SCOPE("Transform::FlushScene");
    world_pose_detail::PrepareAiWorldMath();
    auto& worklist = s_transformTraversal.sceneWorklist;
    const auto rootsProfile = profiler::Profiler::BeginScope(
        profiler::ProfilerMarker("Transform::CollectRoots"));
    worklist.Collect(scene);
    profiler::Profiler::EndScope(rootsProfile);
    {
        FBZZ_PROFILE_SCOPE("Transform::PropagateScene");
        worklist.Propagate([](GameObject& object, const Transform* parent) {
            UpdateWorldTransform(object, parent);
        });
    }
}

void FlushWorldTransforms(GameObject& root)
{
    FBZZ_PROFILE_SCOPE("Transform::FlushSubtree");
    world_pose_detail::PrepareAiWorldMath();
    const auto ancestorsProfile = profiler::Profiler::BeginScope(
        profiler::ProfilerMarker("Transform::ResolveAncestors"));
    auto& ancestors = s_transformTraversal.ancestors;
    ancestors.clear();
    for (GameObject* parent = root.GetParent(); parent; parent = parent->GetParent())
        ancestors.push_back(parent);
    for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
        const GameObject* parent = (*it)->GetParent();
        UpdateWorldTransform(**it, parent ? &parent->transform : nullptr);
    }
    ancestors.clear();
    profiler::Profiler::EndScope(ancestorsProfile);
    FBZZ_PROFILE_SCOPE("Transform::PropagateSubtree");
    const GameObject* parent = root.GetParent();
    UpdateWorldTransform(root, parent ? &parent->transform : nullptr);
    if (root.GetChildCount() == 0) return;
    auto& queue = s_transformTraversal.pending;
    queue.clear();
    for (int i = 0; i < root.GetChildCount(); ++i)
        if (auto* child = root.GetChild(i)) queue.push_back(child);
    UpdateWorldTransformQueue(queue);
}

void SetWorldPose(Transform& tf, const Transform* parent,
                  const math::Vector3& worldPosition,
                  const math::Quaternion& worldRotation,
                  const math::Vector3& worldScale)
{
    /// @note 親のゼロスケール軸で除算せず、有限なローカル値へ倒す。
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

} /// @note namespace fbzz::scene
