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
#include "Engine/Scene/Systems/ParticleSimulationSystem.hpp"
#include <algorithm>
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

static void UpdatePresentationTransform(GameObject& go,
                                        const Transform* parentTransform,
                                        float physicsAlpha,
                                        bool simulating)
{
    Transform& tf = go.transform;
    const auto* rb = go.GetComponent<RigidBodyComponent>();
    const bool interpolatePhysics = simulating && rb && rb->enabled && rb->rigidBody &&
        !rb->rigidBody->IsStatic() && rb->hasPhysicsPoseHistory;

    if (interpolatePhysics) {
        const float alpha = std::clamp(physicsAlpha, 0.0f, 1.0f);
        tf.presentationWorldPosition = math::Vector3::Lerp(
            rb->previousPhysicsPosition, rb->currentPhysicsPosition, alpha);
        tf.presentationWorldRotation = math::Quaternion::Slerp(
            rb->previousPhysicsRotation, rb->currentPhysicsRotation, alpha).Normalized();
        tf.presentationWorldScale = tf.worldScale;
        return;
    }

    if (!parentTransform) {
        tf.presentationWorldPosition = tf.worldPosition;
        tf.presentationWorldRotation = tf.worldRotation;
        tf.presentationWorldScale = tf.worldScale;
        return;
    }

    const math::Vector3 scaledLocal = {
        tf.position.x * parentTransform->presentationWorldScale.x,
        tf.position.y * parentTransform->presentationWorldScale.y,
        tf.position.z * parentTransform->presentationWorldScale.z
    };
    tf.presentationWorldRotation =
        (parentTransform->presentationWorldRotation * tf.rotation).Normalized();
    tf.presentationWorldPosition = parentTransform->presentationWorldPosition +
        parentTransform->presentationWorldRotation * scaledLocal;
    tf.presentationWorldScale = {
        parentTransform->presentationWorldScale.x * tf.scale.x,
        parentTransform->presentationWorldScale.y * tf.scale.y,
        parentTransform->presentationWorldScale.z * tf.scale.z
    };
}

static void UpdatePresentationTransforms(Scene& scene, float physicsAlpha, bool simulating)
{
    // 親の補間姿勢を子へ継承するため、ワールド Transform と同じ BFS 順で処理する。
    std::queue<GameObject*> queue;
    for (GameObject& go : scene.GameObjects())
        if (!go.GetParent())
            queue.push(&go);

    while (!queue.empty()) {
        GameObject* go = queue.front();
        queue.pop();
        const Transform* parentTransform = nullptr;
        if (auto* parent = go->GetParent())
            parentTransform = &parent->transform;
        UpdatePresentationTransform(*go, parentTransform, physicsAlpha, simulating);
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

    if (ctx.simulating && GetPhase() == Phase::PrePhysics) {
        // 初回 fixed step より前にワールド姿勢で履歴を初期化する。
        // WHY FixedScript より後の PhysicsSystem で初期化すると、最初の OnFixedUpdate が
        //     RigidBody へ設定した姿勢を Scene の初期値で上書きしてしまうため。
        for (EntityID id : scene.GetEntities<RigidBodyComponent>()) {
            GameObject* go = scene.GetGameObject(id);
            auto* rb = scene.GetComponent<RigidBodyComponent>(id);
            if (!go || !rb || !rb->enabled || !rb->rigidBody || rb->hasPhysicsPoseHistory)
                continue;
            rb->rigidBody->SetPosition(go->transform.worldPosition);
            rb->rigidBody->SetRotation(go->transform.worldRotation);
            rb->ResetPhysicsPoseHistory(
                go->transform.worldPosition, go->transform.worldRotation);
        }
    }
}

OrderingHints TransformPresentationPostPhysics::GetOrder() const
{
    return OrderingHints{}.After<TransformPostPhysics>();
}

void TransformPresentationPostPhysics::Update(SystemContext& ctx)
{
    UpdatePresentationTransforms(ctx.scene, ctx.physicsAlpha, ctx.simulating);
}

OrderingHints TransformPresentationLateUpdate::GetOrder() const
{
    // ParticleSimulation は Animator / IK より後で、登録順により Camera / Billboard /
    // Presentation よりも後段にある。ここを最後の Transform 消費者として固定する。
    return OrderingHints{}
        .After<PresentationSystem>()
        .After<ParticleSimulationSystem>();
}

void TransformPresentationLateUpdate::Update(SystemContext& ctx)
{
    UpdatePresentationTransforms(ctx.scene, ctx.physicsAlpha, ctx.simulating);
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

} // namespace fbzz::scene
