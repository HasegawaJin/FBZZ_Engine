// FBZZ Engine
// TransformSystem.cpp | fbzz::scene
// 親子階層を BFS で走査し、world 空間の position / rotation を計算する
#include "engine/Scene/Systems/TransformSystem.hpp"
#include "engine/Scene/Scene.hpp"
#include <queue>

namespace fbzz::scene {

static void UpdateWorldTransform(GameObject& go, const Transform* parentTransform) {
    Transform& tf = go.transform;

    if (parentTransform) {
        // 親の scale でローカル位置をスケール → 親の回転を掛ける → 親のワールド位置を足す
        math::Vector3 scaledLocal = {
            tf.localPosition.x * parentTransform->localScale.x,
            tf.localPosition.y * parentTransform->localScale.y,
            tf.localPosition.z * parentTransform->localScale.z
        };
        tf.rotation = (parentTransform->rotation * tf.localRotation).Normalized();
        tf.position = parentTransform->position + parentTransform->rotation * scaledLocal;
    } else {
        tf.rotation = tf.localRotation;
        tf.position = tf.localPosition;
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
