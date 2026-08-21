// FBZZ Engine
// NavMeshPatrolSystem.cpp | fbzz::scene
// NavMeshPatrolComponent のウェイポイントを順送りし、同一 GO の NavMeshAgentComponent へ
// SetDestination する。到達判定そのものは NavigationSystem (destinationReached) に委ねる。
#include "Engine/Scene/Systems/NavMeshPatrolSystem.hpp"
#include "Engine/Core/Scheduler/SystemContext.hpp"
#include "Engine/Scene/Systems/NavMeshSensorSystem.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Components/NavMeshPatrolComponent.hpp"
#include "Engine/Scene/Components/NavMeshAgentComponent.hpp"

namespace fbzz::scene {

namespace {

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

// ウェイポイント i の待機時間を返す。waypointWaitTimes が使えなければ共通 waitTime を返す。
float GetWaitTime(const NavMeshPatrolComponent& patrol, size_t i)
{
    if (i < patrol.waypointWaitTimes.size())
        return patrol.waypointWaitTimes[i];
    return patrol.waitTime;
}

// ウェイポイント i の移動速度を返す (0 以下 = Agent のデフォルト速度を使う)。
float GetWaypointSpeed(const NavMeshPatrolComponent& patrol, size_t i)
{
    if (i < patrol.waypointSpeeds.size())
        return patrol.waypointSpeeds[i];
    return 0.0f;
}

// Agent の移動速度を一時的に上書きする。speed <= 0 なら元の maxSpeed を維持する。
// WHY: patrolDefaultSpeed を保存して次のウェイポイントで復元する方式は
//      複数フレームをまたぐ状態管理が複雑になるため、
//      Patrol が毎フレーム maxSpeed を必要な値にクランプする方式を使う。
void ApplyWaypointSpeed(NavMeshAgentComponent& agent, float speed)
{
    if (speed > 0.0f)
        agent.maxSpeed = speed;
}

} // namespace

ComponentAccess NavMeshPatrolSystem::GetAccess() const
{
    return ComponentAccess{}
        .Reads<NavMeshPatrolComponent, NavMeshSensorComponent>()
        .Writes<NavMeshAgentComponent>();
}

OrderingHints NavMeshPatrolSystem::GetOrder() const
{
    return OrderingHints{}.After<NavMeshSensorSystem>();
}

void NavMeshPatrolSystem::Update(SystemContext& ctx)
{
    Scene& scene = ctx.scene;
    const float dt = ctx.dt;
    for (EntityID eid : scene.GetEntities<NavMeshPatrolComponent>()) {
        auto* patrol = scene.GetComponent<NavMeshPatrolComponent>(eid);
        auto* agent  = scene.GetComponent<NavMeshAgentComponent>(eid);
        auto* go     = scene.GetGameObject(eid);
        if (!patrol || !agent || !go || !go->activeInHierarchy()
            || !patrol->enabled || !agent->enabled) continue;
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
            ApplyWaypointSpeed(*agent, GetWaypointSpeed(*patrol, patrol->currentIndex));
            continue;
        }

        if (!patrol->started) {
            patrol->started = true;
            agent->SetDestination(patrol->waypoints[patrol->currentIndex]);
            ApplyWaypointSpeed(*agent, GetWaypointSpeed(*patrol, patrol->currentIndex));
            continue;
        }

        // NavigationSystem が到達済みと判定したら、待機 or 次のウェイポイントへ進む。
        if (agent->destinationReached && !agent->hasDestination) {
            const float wt = GetWaitTime(*patrol, patrol->currentIndex);
            if (wt > 0.0f) {
                patrol->waiting   = true;
                patrol->waitTimer = wt;
            } else {
                AdvanceWaypoint(*patrol);
                agent->SetDestination(patrol->waypoints[patrol->currentIndex]);
                ApplyWaypointSpeed(*agent, GetWaypointSpeed(*patrol, patrol->currentIndex));
            }
        }
    }
}

} // namespace fbzz::scene
