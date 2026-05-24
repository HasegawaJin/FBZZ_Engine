// FBZZ Engine
// TransformSystem.cpp | fbzz::scene
// 親子階層のワールド Transform 更新
// ルートから BFS で辿り、ローカル値からワールドの position / rotation を再計算する。
// 循環しない親子関係を前提にする。
#include "Engine/Scene/Systems/TransformSystem.hpp"
#include "Engine/Scene/Scene.hpp"
#include <queue>

namespace fbzz::scene {

static void UpdateWorldTransform(GameObject& go, const Transform* parentTransform) {
    Transform& tf = go.transform;

    if (parentTransform) {
        math::Vector3 scaledLocal = {
            tf.localPosition.x * parentTransform->worldScale.x,
            tf.localPosition.y * parentTransform->worldScale.y,
            tf.localPosition.z * parentTransform->worldScale.z
        };
        tf.rotation   = (parentTransform->rotation * tf.localRotation).Normalized();
        tf.position   = parentTransform->position + parentTransform->rotation * scaledLocal;
        tf.worldScale = {
            parentTransform->worldScale.x * tf.localScale.x,
            parentTransform->worldScale.y * tf.localScale.y,
            parentTransform->worldScale.z * tf.localScale.z
        };
    } else {
        tf.rotation   = tf.localRotation;
        tf.position   = tf.localPosition;
        tf.worldScale = tf.localScale;
    }
}

void TransformSystem(Scene& scene) {
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
}

} // namespace fbzz::scene
