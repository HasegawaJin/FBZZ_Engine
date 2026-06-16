// FBZZ Engine
// NavMeshPatrolSystem.cpp | fbzz::scene
// NavMeshPatrolComponent のウェイポイントを順送りし、同一 GO の NavMeshAgentComponent へ
// SetDestination する。到達判定そのものは NavigationSystem (destinationReached) に委ねる。
#include "Engine/Scene/Systems/NavMeshPatrolSystem.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Components/NavMeshPatrolComponent.hpp"
#include "Engine/Scene/Components/NavMeshAgentComponent.hpp"

namespace fbzz::scene {

namespace {

// 現在のウェイポイントを Mode に応じて次へ進める。
void AdvanceWaypoint(NavMeshPatrolComponent& patrol)
{
    const size_t count = patrol.waypoints.size();
    if (count <= 1) return;

    if (patrol.mode == NavMeshPatrolComponent::Mode::LOOP) {
        patrol.currentIndex = (patrol.currentIndex + 1) % count;
        return;
    }

    // PING_PONG: 端に達したら進行方向を反転する。
    if (patrol.currentIndex == 0) patrol.direction = 1;
    else if (patrol.currentIndex == count - 1) patrol.direction = -1;
    patrol.currentIndex = static_cast<size_t>(static_cast<int>(patrol.currentIndex) + patrol.direction);
}

} // namespace

void NavMeshPatrolSystem(Scene& scene, float dt)
{
    for (EntityID eid : scene.GetEntities<NavMeshPatrolComponent>()) {
        auto* patrol = scene.GetComponent<NavMeshPatrolComponent>(eid);
        auto* agent  = scene.GetComponent<NavMeshAgentComponent>(eid);
        if (!patrol || !agent || !patrol->enabled || !agent->enabled) continue;
        if (patrol->waypoints.empty()) continue;
        if (patrol->currentIndex >= patrol->waypoints.size()) patrol->currentIndex = 0;

        // 追跡対象に切り替わっている間 (Target Follow 中) は巡回を中断する。
        if (agent->target.IsValid()) continue;

        if (patrol->waiting) {
            patrol->waitTimer -= dt;
            if (patrol->waitTimer > 0.0f) continue;
            patrol->waiting = false;
            AdvanceWaypoint(*patrol);
            agent->SetDestination(patrol->waypoints[patrol->currentIndex]);
            continue;
        }

        if (!patrol->started) {
            patrol->started = true;
            agent->SetDestination(patrol->waypoints[patrol->currentIndex]);
            continue;
        }

        // NavigationSystem が到達済みと判定したら、待機 or 次のウェイポイントへ進む。
        if (agent->destinationReached && !agent->hasDestination) {
            if (patrol->waitTime > 0.0f) {
                patrol->waiting   = true;
                patrol->waitTimer = patrol->waitTime;
            } else {
                AdvanceWaypoint(*patrol);
                agent->SetDestination(patrol->waypoints[patrol->currentIndex]);
            }
        }
    }
}

} // namespace fbzz::scene
