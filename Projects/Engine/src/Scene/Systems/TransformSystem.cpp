// FBZZ Engine
// TransformSystem.cpp | fbzz::scene
// 親子階層のワールド Transform 更新
// ルートから BFS で辿り、ローカル値からワールドの position / rotation を再計算する。
// 循環しない親子関係を前提にする。
#include "Engine/Scene/Systems/TransformSystem.hpp"
#include "Engine/Core/Scheduler/SystemContext.hpp"
#include "Engine/Scene/Components/RigidBodyComponent.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Systems/GameplayComponentSystems.hpp"
#include <cmath>
#include <queue>

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

static void ApplyPhysicsInterpolation(Scene& scene, float alpha)
{
    // WHY: fixed step 後の確定 Transform を直接補間すると次の PhysicsSystem が
    //      テレポートと誤認するため、local 値は触らず world 値だけを描画用に更新する。
    //      次フレームの TransformPrePhysics が local → world を再計算し、Physics には
    //      補間前の確定姿勢が戻る。
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
    // ルート (親なし) から BFS で子孫を更新する
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

        UpdateWorldTransform(*go, parentTf);

        for (int i = 0; i < go->GetChildCount(); ++i)
            if (auto* child = go->GetChild(i))
                queue.push(child);
    }

    if (ctx.simulating && GetPhase() == Phase::LateUpdate)
        ApplyPhysicsInterpolation(scene, ctx.interpolationAlpha);

    if (ctx.simulating && GetPhase() == Phase::PrePhysics) {
        // 初回 fixed step より前にワールド姿勢で履歴を初期化する。
        // WHY FixedScript より後の PhysicsSystem で初期化すると、最初の OnFixedUpdate が
        //     RigidBody へ設定した姿勢を Scene の初期値で上書きしてしまうため。
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
    // スケジューラを経由しないため SystemContext は不要。
    // BFS でルートから辿り、TransformSystem::Update と同じ計算を実行する。
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

        UpdateWorldTransform(*go, parentTf);

        for (int i = 0; i < go->GetChildCount(); ++i)
            if (auto* child = go->GetChild(i))
                queue.push(child);
    }
}

void SetWorldPose(GameObject& go,
                  const math::Vector3& worldPosition,
                  const math::Quaternion& worldRotation,
                  const math::Vector3& worldScale)
{
    // 0 スケールの親で割らない。親が潰れている軸は local を 0 に倒し、
    // 「無限大が Transform に混ざって以降のフレームが全部 NaN になる」壊れ方を避ける。
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

} // namespace fbzz::scene
