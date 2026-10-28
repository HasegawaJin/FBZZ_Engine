/// @file    LifetimeSystem.cpp
/// @brief   寿命を減算し、Script と GameObject の遅延破棄を Cleanup で確定する。
/// @author  Hasegawa Jin
/// @date    2026-06-10
///
/// @note 0 以下になったら GameObject::Destroy でキューに積む。
/// @note FlushDestroyQueue が実際の削除を行う。
#include <Engine/Scene/Systems/LifetimeSystem.hpp>
#include "Engine/Core/Scheduler/SystemContext.hpp"
#include "Engine/Scene/Systems/LifetimeSystem.hpp"
#include <Engine/Scene/Components/LifetimeComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include "../Managed/ManagedObjectControls.hpp"

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
        auto* go = scene.GetGameObject(id);
        if (!lc || !go || !go->activeInHierarchy()) continue;
        if (!lc->enabled) continue;
        lc->remaining -= dt;
        if (lc->remaining <= 0.0f) {
            GameObject::Destroy(*go, 0.0f);
        }
    }
}

OrderingHints FlushDestroyQueueSystem::GetOrder() const
{
    return OrderingHints{}.After<LifetimeSystem>();
}

void FlushDestroyQueueSystem::Update(SystemContext& ctx)
{
    managed_detail::ManagedObjectControls::FlushScriptRemovals(ctx.scene);
    ctx.scene.FlushDestroyQueue(ctx.dt);
}

} /// @note namespace fbzz::scene
