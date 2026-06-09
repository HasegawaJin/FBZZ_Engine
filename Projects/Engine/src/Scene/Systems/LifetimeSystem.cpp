// FBZZ Engine
// LifetimeSystem.cpp | fbzz::scene
// LifetimeComponent を持つ GO の残り寿命を毎フレーム減算し、
// 0 以下になったら GameObject::Destroy でキューに積む。
// FlushDestroyQueue が実際の削除を行う。
#include <Engine/Scene/Systems/LifetimeSystem.hpp>
#include <Engine/Scene/Components/LifetimeComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>

namespace fbzz::scene {

void LifetimeSystem(Scene& scene, float dt)
{
    for (EntityID id : scene.GetEntities<LifetimeComponent>()) {
        auto* lc = scene.GetComponent<LifetimeComponent>(id);
        if (!lc) continue;
        if (!lc->enabled) continue;
        lc->remaining -= dt;
        if (lc->remaining <= 0.0f) {
            auto* go = scene.GetGameObject(id);
            if (go) GameObject::Destroy(*go, 0.0f);
        }
    }
}

} // namespace fbzz::scene
