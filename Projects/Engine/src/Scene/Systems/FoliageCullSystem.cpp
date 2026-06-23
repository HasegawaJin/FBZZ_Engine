// FBZZ Engine
// FoliageCullSystem.cpp | fbzz::scene
// FoliageComponent.ChildEntry.colliderCullDistance > 0 の stamp 子 GO に対し、
// メインカメラ(CameraComponent.isMain == true)との距離を見て
// BoxColliderComponent.enabled を切り替える。
// WHY: stamp 木が多いシーンでは遠距離の静的 OBB コライダーが PhysicsSystem の
//      broadphase ペア数を増やす。動的に enabled を落とすことでペアを削減できる。
//      GO の破棄/再生成は行わないため FoliageBakeSystem の needsBakeChildren は発火しない。
#include "Engine/Scene/Systems/FoliageCullSystem.hpp"
#include "Engine/Core/Scheduler/SystemContext.hpp"
#include "Engine/Scene/Systems/FoliageBakeSystem.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/GameObject.hpp"
#include "Engine/Scene/Components/CameraComponent.hpp"
#include "Engine/Scene/Components/ColliderComponent.hpp"
#include "Engine/Scene/Components/FoliageComponent.hpp"
#include <Math/Vector3.hpp>

namespace fbzz::scene {

ComponentAccess FoliageCullSystem::GetAccess() const
{
    return ComponentAccess{}
        .Reads<FoliageComponent, CameraComponent>()
        .Writes<BoxColliderComponent>();
}

OrderingHints FoliageCullSystem::GetOrder() const
{
    return OrderingHints{}.After<FoliageBakeSystem>();
}

void FoliageCullSystem::Update(SystemContext& ctx)
{
    Scene& scene = ctx.scene;
    // メインカメラの world 位置を基準点とする。
    // WHY: TransformSystem 実行後に呼ぶため worldPosition は最新値になっている。
    math::Vector3 refPos = {};
    bool hasRef = false;
    for (EntityID cid : scene.GetEntities<CameraComponent>()) {
        const auto* cam = scene.GetComponent<CameraComponent>(cid);
        const auto* go  = scene.GetGameObject(cid);
        if (cam && cam->isMain && cam->enabled && go) {
            refPos = go->transform.worldPosition;
            hasRef = true;
            break;
        }
    }
    if (!hasRef) return;

    for (EntityID eid : scene.GetEntities<FoliageComponent>()) {
        const auto* foliage = scene.GetComponent<FoliageComponent>(eid);
        if (!foliage || !foliage->enabled) continue;

        for (const auto& child : foliage->childEntities) {
            if (child.colliderCullDistance <= 0.0f) continue;

            auto* childGO = scene.GetGameObject(child.entityID);
            if (!childGO) continue;

            auto* box = childGO->GetComponent<BoxColliderComponent>();
            if (!box) continue;

            const math::Vector3& p = childGO->transform.worldPosition;
            const float dx = p.x - refPos.x;
            const float dy = p.y - refPos.y;
            const float dz = p.z - refPos.z;
            const float distSq = dx*dx + dy*dy + dz*dz;
            const float cullSq = child.colliderCullDistance * child.colliderCullDistance;

            box->enabled = (distSq <= cullSq);
        }
    }
}

} // namespace fbzz::scene
