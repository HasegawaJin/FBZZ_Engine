/// @file    NavMeshPatrolSystem.cpp
/// @brief   Resolve patrol points and submit throttled NavMesh destinations.
/// @author  Hasegawa Jin
/// @date    2026-06-17
#include <Engine/Scene/Systems/NavMeshPatrolSystem.hpp>

#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Scene/Components/NavMeshAgentComponent.hpp>
#include <Engine/Scene/Components/NavMeshPatrolComponent.hpp>
#include <Engine/Scene/Components/NavMeshSensorComponent.hpp>
#include <Engine/Scene/Components/TargetPointComponent.hpp>
#include <Engine/Scene/NavigationControlPoints.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/BehaviorTreeComponent.hpp>
#include <Engine/Scene/Systems/NavMeshSensorSystem.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::scene {
namespace {

void AdvanceWaypoint(NavMeshPatrolComponent& patrol, std::size_t count)
{
    if (count <= 1) return;
    if (patrol.mode == NavMeshPatrolComponent::Mode::LOOP) {
        patrol.currentIndex = (patrol.currentIndex + 1) % count;
        return;
    }

    if (patrol.currentIndex == 0) patrol.direction = 1;
    else if (patrol.currentIndex == count - 1) patrol.direction = -1;
    patrol.currentIndex = static_cast<std::size_t>(
        static_cast<int>(patrol.currentIndex) + patrol.direction);
}

void ApplyWaypointSpeed(NavMeshAgentComponent& agent, float speed)
{
    if (speed > 0.0f) agent.maxSpeed = speed;
}

const NavMeshPatrolPointValue* SelectCurrentPoint(
    NavMeshPatrolComponent& patrol,
    const NavMeshPatrolPointSnapshot& snapshot,
    bool hasStableBindings)
{
    if (snapshot.points.empty()) return nullptr;
    if (!hasStableBindings) {
        if (patrol.currentIndex >= snapshot.points.size()) patrol.currentIndex = 0;
        return &snapshot.points[patrol.currentIndex];
    }

    if (patrol.currentWaypointId == 0) {
        if (patrol.currentIndex >= snapshot.points.size()) patrol.currentIndex = 0;
        patrol.currentWaypointId = snapshot.points[patrol.currentIndex].stableId;
        return &snapshot.points[patrol.currentIndex];
    }

    const auto found = std::find_if(snapshot.points.begin(), snapshot.points.end(),
        [&patrol](const NavMeshPatrolPointValue& point) {
            return point.stableId == patrol.currentWaypointId;
        });
    if (found == snapshot.points.end()) return nullptr;
    patrol.currentIndex = static_cast<std::size_t>(found - snapshot.points.begin());
    return &*found;
}

void SyncCurrentBinding(NavMeshPatrolComponent& patrol,
    const NavMeshPatrolPointSnapshot& snapshot, bool hasStableBindings)
{
    if (!hasStableBindings || snapshot.points.empty()
        || patrol.currentIndex >= snapshot.points.size()) return;
    patrol.currentWaypointId = snapshot.points[patrol.currentIndex].stableId;
}

void SubmitDestination(NavMeshPatrolComponent& patrol,
    NavMeshAgentComponent& agent, const NavMeshPatrolPointValue& point,
    bool trackLiveBinding)
{
    agent.SetDestination(point.worldPosition);
    ApplyWaypointSpeed(agent, point.speed);
    if (trackLiveBinding) {
        patrol.lastSubmittedPosition = point.worldPosition;
        patrol.hasSubmittedPosition = true;
        patrol.liveRepathTimer = 0.0f;
    }
}

void StopPatrolDestination(NavMeshPatrolComponent& patrol, NavMeshAgentComponent& agent)
{
    agent.Stop();
    patrol.started = false;
    patrol.waiting = false;
    patrol.waitTimer = 0.0f;
    patrol.hasSubmittedPosition = false;
    patrol.liveRepathTimer = 0.0f;
}

void ReportInvalidPatrolInput(NavMeshPatrolComponent& patrol, EntityID entity,
    const NavMeshPatrolPointSnapshot& snapshot)
{
    if (snapshot.inputSignature == patrol.lastDiagnosticSignature) return;
    patrol.lastDiagnosticSignature = snapshot.inputSignature;
    FBZZ_LOG_ERROR("NavMeshPatrolSystem: invalid patrol input on entity %u:%u (status=%u, resolve=%u).",
        entity.index, entity.generation, static_cast<unsigned>(snapshot.status),
        static_cast<unsigned>(snapshot.resolveStatus));
}

} /// @note namespace

ComponentAccess NavMeshPatrolSystem::GetAccess() const
{
    return ComponentAccess{}
        .Reads<NavMeshSensorComponent, TargetPointComponent, BehaviorTreeComponent>()
        .Writes<NavMeshPatrolComponent, NavMeshAgentComponent, Transform>();
}

OrderingHints NavMeshPatrolSystem::GetOrder() const
{
    return OrderingHints{}.After<NavMeshSensorSystem>();
}

void NavMeshPatrolSystem::Update(SystemContext& ctx)
{
    Scene& scene = ctx.scene;
    if (scene.GetEntities<NavMeshPatrolComponent>().empty()) return;
    const float dt = ctx.dt;
    FlushWorldTransforms(scene);
    for (const EntityID eid : scene.GetEntities<NavMeshPatrolComponent>()) {
        auto* patrol = scene.GetComponent<NavMeshPatrolComponent>(eid);
        auto* agent = scene.GetComponent<NavMeshAgentComponent>(eid);
        auto* go = scene.GetGameObject(eid);
        if (!patrol || !agent || !go || !go->activeInHierarchy()
            || !patrol->enabled || !agent->enabled) continue;
        if (agent->target.IsAssigned()) continue;
        /// @note A loaded behavior tree owns route activation; its Patrol action retains permission between decision ticks.
        if (const auto* bt = scene.GetComponent<BehaviorTreeComponent>(eid); bt && bt->runtime && !bt->patrolRequested) continue;

        const NavMeshPatrolPointSnapshot snapshot = ResolveNavMeshPatrolPoints(
            scene, eid, *patrol, ControlPointReadContext::RUNTIME);
        if (!snapshot.valid) {
            ReportInvalidPatrolInput(*patrol, eid, snapshot);
            StopPatrolDestination(*patrol, *agent);
            continue;
        }
        patrol->lastDiagnosticSignature = 0;
        if (snapshot.points.empty()) {
            if (patrol->pointSourceMode == ControlPointSourceMode::POINT_OBJECTS)
                StopPatrolDestination(*patrol, *agent);
            continue;
        }
        const bool hasStableBindings = patrol->pointSourceMode == ControlPointSourceMode::POINT_OBJECTS;
        const bool trackLiveBinding = hasStableBindings
            && patrol->pointRuntimeMode == ControlPointRuntimeMode::LIVE;
        const NavMeshPatrolPointValue* current = SelectCurrentPoint(
            *patrol, snapshot, hasStableBindings);
        if (!current) {
            StopPatrolDestination(*patrol, *agent);
            continue;
        }

        if (patrol->waiting) {
            patrol->waitTimer -= dt;
            if (patrol->waitTimer > 0.0f) continue;
            patrol->waiting = false;
            AdvanceWaypoint(*patrol, snapshot.points.size());
            SyncCurrentBinding(*patrol, snapshot, hasStableBindings);
            current = SelectCurrentPoint(*patrol, snapshot, hasStableBindings);
            if (current) SubmitDestination(*patrol, *agent, *current, trackLiveBinding);
            continue;
        }

        if (!patrol->started) {
            patrol->started = true;
            SubmitDestination(*patrol, *agent, *current, trackLiveBinding);
            continue;
        }

        if (trackLiveBinding) {
            patrol->liveRepathTimer += dt;
            const float minimumInterval = (std::max)(0.0f, patrol->liveRepathInterval);
            const float minimumDistance = (std::max)(0.0f, patrol->liveRepathDistance);
            const math::Vector3 delta = current->worldPosition - patrol->lastSubmittedPosition;
            const float distanceSquared = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
            if (patrol->hasSubmittedPosition && patrol->liveRepathTimer >= minimumInterval
                && std::isfinite(distanceSquared) && distanceSquared > 0.0f
                && distanceSquared >= minimumDistance * minimumDistance) {
                SubmitDestination(*patrol, *agent, *current, true);
                continue;
            }
        }

        if (agent->destinationReached && !agent->hasDestination) {
            if (current->waitTime > 0.0f) {
                patrol->waiting = true;
                patrol->waitTimer = current->waitTime;
            } else {
                AdvanceWaypoint(*patrol, snapshot.points.size());
                SyncCurrentBinding(*patrol, snapshot, hasStableBindings);
                current = SelectCurrentPoint(*patrol, snapshot, hasStableBindings);
                if (current) SubmitDestination(*patrol, *agent, *current, trackLiveBinding);
            }
        }
    }
}

} /// @note namespace fbzz::scene
