// FBZZ Engine
// LifetimeSystem.cpp | fbzz::scene
// LifetimeComponent を持つ GO の残り寿命を毎フレーム減算し、
// 0 以下になったら GameObject::Destroy でキューに積む。
// FlushDestroyQueue が実際の削除を行う。
#include <Engine/Scene/Systems/LifetimeSystem.hpp>
#include "Engine/Core/Scheduler/SystemContext.hpp"
#include "Engine/Scene/Systems/LifetimeSystem.hpp"
#include <Engine/Scene/Components/LifetimeComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>

namespace fbzz::scene {

ComponentAccess LifetimeSystem::GetAccess() const
{
    return ComponentAccess{}
        .Reads<LifetimeComponent>()
        .Writes<LifetimeComponent>();
}

void LifetimeSystem::Update(SystemContext& ctx)
{
    Scene& scene = ctx.scene;
    const float dt = ctx.dt;
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

OrderingHints FlushDestroyQueueSystem::GetOrder() const
{
    return OrderingHints{}.After<LifetimeSystem>();
}

void FlushDestroyQueueSystem::Update(SystemContext& ctx)
{
    ctx.scene.FlushDestroyQueue(ctx.dt);
}

} // namespace fbzz::scene
