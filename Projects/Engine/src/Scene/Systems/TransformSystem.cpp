/// @file    TransformSystem.cpp
/// @brief   親子階層のワールド Transform 更新。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// @note GameObject の親子関係は循環しないことを前提にする。
#include "Engine/Scene/Systems/TransformSystem.hpp"
#include "Engine/Core/Scheduler/SystemContext.hpp"
#include "Engine/Scene/Components/RigidBodyComponent.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Systems/GameplayComponentSystems.hpp"
#include <cmath>
#include <queue>
#include <vector>

namespace fbzz::scene {

static void UpdateWorldTransform(GameObject& go, const Transform* parentTransform) {
    Transform& tf = go.transform;

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

static void UpdateWorldTransformQueue(std::queue<GameObject*>& queue)
{
    while (!queue.empty()) {
        GameObject* go = queue.front();
        queue.pop();
        const GameObject* parent = go->GetParent();
        UpdateWorldTransform(*go, parent ? &parent->transform : nullptr);
        for (int i = 0; i < go->GetChildCount(); ++i)
            if (auto* child = go->GetChild(i)) queue.push(child);
    }
}

static void ApplyPhysicsInterpolation(Scene& scene, float alpha)
{
    /// @note 物理の確定姿勢を変更しないため、描画補間は world だけへ適用する。
    std::queue<GameObject*> queue;
    for (GameObject& go : scene.GameObjects())
        if (!go.GetParent())
            queue.push(&go);

    while (!queue.empty()) {
        GameObject* go = queue.front();
        queue.pop();

        Transform* parentTf = nullptr;
        if (auto* parent = go->GetParent())
            parentTf = &parent->transform;

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

        for (int i = 0; i < go->GetChildCount(); ++i)
            if (auto* child = go->GetChild(i))
                queue.push(child);
    }
}

ComponentAccess TransformSystem::GetAccess() const
{
    return ComponentAccess{}.Unrestricted();
}

void TransformSystem::Update(SystemContext& ctx)
{
    Scene& scene = ctx.scene;
    FlushWorldTransforms(scene);

    if (ctx.simulating && GetPhase() == Phase::LateUpdate)
        ApplyPhysicsInterpolation(scene, ctx.interpolationAlpha);

    if (ctx.simulating && GetPhase() == Phase::PrePhysics) {
        /// @note 最初の OnFixedUpdate が書いた姿勢を上書きしないよう、物理履歴は FixedScript より前に初期化する。
        for (EntityID id : scene.GetEntities<RigidBodyComponent>()) {
            GameObject* go = scene.GetGameObject(id);
            auto* rb = scene.GetComponent<RigidBodyComponent>(id);
            if (!go || !rb || !rb->enabled || !rb->rigidBody || rb->hasPhysicsSyncState)
                continue;
            rb->rigidBody->SetPosition(go->transform.worldPosition);
            rb->rigidBody->SetRotation(go->transform.worldRotation);
            rb->ResetPhysicsSyncState(
                go->transform.worldPosition, go->transform.worldRotation);
        }
    }
}

void FlushWorldTransforms(Scene& scene)
{
    std::queue<GameObject*> queue;
    for (GameObject& go : scene.GameObjects())
        if (!go.GetParent()) queue.push(&go);
    UpdateWorldTransformQueue(queue);
}

void FlushWorldTransforms(GameObject& root)
{
    std::vector<GameObject*> ancestors;
    for (GameObject* parent = root.GetParent(); parent; parent = parent->GetParent())
        ancestors.push_back(parent);
    for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
        const GameObject* parent = (*it)->GetParent();
        UpdateWorldTransform(**it, parent ? &parent->transform : nullptr);
    }
    const GameObject* parent = root.GetParent();
    UpdateWorldTransform(root, parent ? &parent->transform : nullptr);
    if (root.GetChildCount() == 0) return;
    std::queue<GameObject*> queue;
    for (int i = 0; i < root.GetChildCount(); ++i)
        if (auto* child = root.GetChild(i)) queue.push(child);
    UpdateWorldTransformQueue(queue);
}

void SetWorldPose(GameObject& go,
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

    Transform& tf = go.transform;
    tf.worldPosition = worldPosition;
    tf.worldRotation = worldRotation.Normalized();
    tf.worldScale    = worldScale;

    if (const GameObject* parent = go.GetParent()) {
        const Transform& pt = parent->transform;
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

} /// @note namespace fbzz::scene
