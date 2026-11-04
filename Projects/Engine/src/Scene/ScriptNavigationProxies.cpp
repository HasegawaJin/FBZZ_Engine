/// @file    ScriptNavigationProxies.cpp
/// @brief   Navigation 領域の Script 転送と関連する内部ヘルパー。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include "ScriptProxyCommon.hpp"
#include "ScriptControlPointProxyHelpers.hpp"
#include <Engine/Scene/Script.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/NavigationControlPoints.hpp>
#include <Engine/Scene/Components/NavMeshAgentComponent.hpp>
#include <Engine/Scene/Components/NavMeshSurfaceComponent.hpp>
#include <Engine/Scene/Components/NavMeshSensorComponent.hpp>
#include <Engine/Scene/Components/NavMeshPatrolComponent.hpp>
#include <limits>
#include <type_traits>
#include <string_view>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <Engine/Scene/Systems/TransformSystem.hpp>
namespace fbzz::scene {
using namespace script_proxy_detail;
namespace {
NavMeshPatrolPointSnapshot ResolveScriptPatrolPoints(
    Scene& scene, EntityID owner, const NavMeshPatrolComponent& patrol)
{
    FlushWorldTransforms(scene);
    return ResolveNavMeshPatrolPoints(scene, owner, patrol, ScriptControlPointReadContext());
}

/// @brief Resolve a Line's selected numeric source after synchronizing its referenced transforms.

bool MaterializePatrolPoints(const NavMeshPatrolPointSnapshot& snapshot,
    std::vector<math::Vector3>& positions, std::vector<float>& waitTimes,
    std::vector<float>& speeds)
{
    if (!snapshot.valid) return false;
    std::vector<math::Vector3> nextPositions;
    std::vector<float> nextWaitTimes;
    std::vector<float> nextSpeeds;
    nextPositions.reserve(snapshot.points.size());
    nextWaitTimes.reserve(snapshot.points.size());
    nextSpeeds.reserve(snapshot.points.size());
    for (const NavMeshPatrolPointValue& point : snapshot.points) {
        nextPositions.push_back(point.worldPosition);
        nextWaitTimes.push_back(point.waitTime);
        nextSpeeds.push_back(point.speed);
    }
    positions = std::move(nextPositions);
    waitTimes = std::move(nextWaitTimes);
    speeds = std::move(nextSpeeds);
    return true;
}


} /// @note namespace
#undef GIZMO_ASSERT

/// @name ScriptNavigationProxy 実装
/// @note フリー関数は Script::m_gameObject (protected) へアクセスできない (friend 指定は
/// @note ScriptNavigationProxy のメンバー関数にのみ及ぶ)。既存の SelfComponent<T>() (public な
/// @note Script::GetComponent<T>() 経由) を再利用する。
namespace {
NavMeshAgentComponent* SelfNavAgent(const Script* script)
{
    return SelfComponent<NavMeshAgentComponent>(script);
}
} ///< @note namespace


void ScriptNavigationProxy::SetDestination(const math::Vector3& worldPos) const
{
    if (auto* agent = SelfNavAgent(script)) agent->SetDestination(worldPos);
}


void ScriptNavigationProxy::CancelPath() const
{
    if (auto* agent = SelfNavAgent(script)) agent->Stop();
}


void ScriptNavigationProxy::Pause() const
{
    if (auto* agent = SelfNavAgent(script)) agent->isStopped = true;
}


void ScriptNavigationProxy::Resume() const
{
    if (auto* agent = SelfNavAgent(script)) agent->isStopped = false;
}


void ScriptNavigationProxy::Warp(const math::Vector3& worldPos) const
{
    if (!script || !script->m_gameObject) return;
    if (auto* agent = SelfNavAgent(script)) agent->Stop();
    script->m_gameObject->transform.position      = worldPos;
    script->m_gameObject->transform.worldPosition = worldPos;
}


void ScriptNavigationProxy::SetTarget(EntityID target, float repathInterval) const
{
    if (auto* agent = SelfNavAgent(script)) agent->SetTarget(target, repathInterval);
}


void ScriptNavigationProxy::SetTarget(const GameObject& target, float repathInterval) const
{
    SetTarget(target.GetID(), repathInterval);
}


void ScriptNavigationProxy::ClearTarget() const
{
    if (auto* agent = SelfNavAgent(script)) agent->ClearTarget();
}


bool ScriptNavigationProxy::IsFollowingTarget() const
{
    auto* agent = SelfNavAgent(script);
    return agent && agent->target.IsAssigned();
}


void ScriptNavigationProxy::BakeNavMesh() const
{
    if (!script || !script->m_scene) return;
    auto* agent = SelfNavAgent(script);
    const int typeId = agent ? agent->agentTypeId : 0;
    for (EntityID eid : script->m_scene->GetEntities<NavMeshSurfaceComponent>()) {
        auto* surf = script->m_scene->GetComponent<NavMeshSurfaceComponent>(eid);
        if (surf && surf->agentTypeId == typeId) surf->needsBake = true;
    }
}


bool ScriptNavigationProxy::IsPaused() const
{
    auto* agent = SelfNavAgent(script);
    return agent && agent->isStopped;
}


bool ScriptNavigationProxy::IsMoving() const
{
    auto* agent = SelfNavAgent(script);
    if (!agent || agent->isStopped) return false;
    return agent->state == NavMeshAgentState::MOVING
        || agent->state == NavMeshAgentState::TRAVERSING_LINK;
}


bool ScriptNavigationProxy::HasPath() const
{
    auto* agent = SelfNavAgent(script);
    return agent && agent->HasPath();
}


bool ScriptNavigationProxy::HasArrived() const
{
    auto* agent = SelfNavAgent(script);
    return agent && agent->destinationReached;
}


float ScriptNavigationProxy::GetRemainingDistance() const
{
    auto* agent = SelfNavAgent(script);
    return agent ? agent->remainingDistance : 0.0f;
}


bool ScriptNavigationProxy::IsStuck() const
{
    auto* agent = SelfNavAgent(script);
    return agent && agent->isStuck;
}


math::Vector3 ScriptNavigationProxy::GetVelocity() const
{
    auto* agent = SelfNavAgent(script);
    return agent ? agent->velocity : math::Vector3::ZERO;
}


void ScriptNavigationProxy::SetSpeed(float speed) const
{
    if (auto* agent = SelfNavAgent(script)) agent->maxSpeed = speed > 0.0f ? speed : 0.0f;
}


float ScriptNavigationProxy::GetSpeed() const
{
    const auto* agent = SelfNavAgent(script);
    return agent ? agent->maxSpeed : 0.0f;
}


void ScriptNavigationProxy::SetAngularSpeed(float degPerSec) const
{
    if (auto* agent = SelfNavAgent(script)) agent->angularSpeedDeg = degPerSec >= 0.0f ? degPerSec : 0.0f;
}


float ScriptNavigationProxy::GetAngularSpeed() const
{
    const auto* agent = SelfNavAgent(script);
    return agent ? agent->angularSpeedDeg : 0.0f;
}


math::Vector3 ScriptNavigationProxy::GetDestination() const
{
    const auto* agent = SelfNavAgent(script);
    return agent && agent->hasDestination ? agent->destination : math::Vector3::ZERO;
}


float ScriptNavigationProxy::GetStoppingDistance() const
{
    const auto* agent = SelfNavAgent(script);
    return agent ? agent->stoppingDistance : 0.0f;
}


void ScriptNavigationProxy::SetStoppingDistance(float distance) const
{
    if (auto* agent = SelfNavAgent(script))
        agent->stoppingDistance = distance > 0.0f ? distance : 0.0f;
}


int ScriptNavigationProxy::GetAreaMask() const
{
    auto* agent = SelfNavAgent(script);
    return agent ? agent->areaMask : -1;
}


void ScriptNavigationProxy::SetAreaMask(int mask) const
{
    if (auto* agent = SelfNavAgent(script)) agent->areaMask = mask;
}


bool ScriptNavigationProxy::CanSeeTarget() const
{
    if (!script || !script->m_gameObject) return false;
    auto* sensor = script->m_gameObject->GetComponent<NavMeshSensorComponent>();
    return sensor && sensor->targetVisible;
}


GameObject* ScriptNavigationProxy::GetDetectedTarget() const
{
    if (!script || !script->m_gameObject) return nullptr;
    auto* sensor = script->m_gameObject->GetComponent<NavMeshSensorComponent>();
    if (!sensor || !sensor->targetVisible) return nullptr;
    return script->m_scene ? script->m_scene->GetGameObject(sensor->detectedTarget) : nullptr;
}

/// @name EntityRef 実装

void ScriptPatrolProxy::SetEnabled(bool enabled) const
{
    if (auto* patrol = SelfComponent<NavMeshPatrolComponent>(script)) patrol->enabled = enabled;
}

void ScriptPatrolProxy::SetMode(ScriptPatrolMode mode) const
{
    if (auto* patrol = SelfComponent<NavMeshPatrolComponent>(script)) {
        patrol->mode = mode == ScriptPatrolMode::PING_PONG
            ? NavMeshPatrolComponent::Mode::PING_PONG
            : NavMeshPatrolComponent::Mode::LOOP;
    }
}

void ScriptPatrolProxy::SetWaitTime(float seconds) const
{
    if (auto* patrol = SelfComponent<NavMeshPatrolComponent>(script)) patrol->waitTime = (std::max)(seconds, 0.0f);
}

void ScriptPatrolProxy::ClearWaypoints() const
{
    if (auto* patrol = SelfComponent<NavMeshPatrolComponent>(script)) {
        patrol->waypoints.clear();
        patrol->waypointWaitTimes.clear();
        patrol->waypointSpeeds.clear();
        patrol->pointSourceMode = ControlPointSourceMode::INLINE;
        patrol->pointRuntimeMode = ControlPointRuntimeMode::LIVE;
        patrol->currentIndex = 0;
        patrol->currentWaypointId = 0;
        patrol->started = false;
        patrol->waiting = false;
    }
}

void ScriptPatrolProxy::AddWaypoint(const math::Vector3& position, float waitTime, float speed) const
{
    auto* patrol = SelfComponent<NavMeshPatrolComponent>(script);
    if (!patrol || !std::isfinite(position.x) || !std::isfinite(position.y)
        || !std::isfinite(position.z) || patrol->waypoints.size() >= MAX_NAV_MESH_PATROL_WAYPOINTS) return;
    const float pointWait = waitTime < 0.0f ? patrol->waitTime : waitTime;
    const float pointSpeed = (std::max)(speed, 0.0f);
    if (!std::isfinite(pointWait) || !std::isfinite(pointSpeed)) return;

    const bool useSelectedValues = patrol->pointSourceMode == ControlPointSourceMode::POINT_OBJECTS
        || (patrol->pointRuntimeMode == ControlPointRuntimeMode::BAKED_STATIC
            && Script::IsInPlayMode());
    if (useSelectedValues) {
        if (!script || !script->m_scene || !script->m_gameObject) return;
        auto snapshot = ResolveScriptPatrolPoints(*script->m_scene,
            script->m_gameObject->GetID(), *patrol);
        std::vector<math::Vector3> positions;
        std::vector<float> waitTimes;
        std::vector<float> speeds;
        if (!MaterializePatrolPoints(snapshot, positions, waitTimes, speeds)
            || positions.size() >= MAX_NAV_MESH_PATROL_WAYPOINTS) return;
        positions.push_back(position);
        waitTimes.push_back(pointWait);
        speeds.push_back(pointSpeed);
        patrol->waypoints = std::move(positions);
        patrol->waypointWaitTimes = std::move(waitTimes);
        patrol->waypointSpeeds = std::move(speeds);
    } else {
        if (patrol->pointSourceMode != ControlPointSourceMode::INLINE) return;
        std::vector<float> waitTimes;
        std::vector<float> speeds;
        waitTimes.reserve(patrol->waypoints.size() + 1);
        speeds.reserve(patrol->waypoints.size() + 1);
        for (std::size_t index = 0; index < patrol->waypoints.size(); ++index) {
            waitTimes.push_back(index < patrol->waypointWaitTimes.size()
                ? patrol->waypointWaitTimes[index] : patrol->waitTime);
            speeds.push_back(index < patrol->waypointSpeeds.size()
                ? patrol->waypointSpeeds[index] : 0.0f);
        }
        patrol->waypoints.push_back(position);
        waitTimes.push_back(pointWait);
        speeds.push_back(pointSpeed);
        patrol->waypointWaitTimes = std::move(waitTimes);
        patrol->waypointSpeeds = std::move(speeds);
    }
    patrol->pointSourceMode = ControlPointSourceMode::INLINE;
    patrol->pointRuntimeMode = ControlPointRuntimeMode::LIVE;
    patrol->currentWaypointId = 0;
    patrol->started = false;
}

bool ScriptPatrolProxy::SetWaypoint(size_t index, const math::Vector3& position) const
{
    auto* patrol = SelfComponent<NavMeshPatrolComponent>(script);
    if (!patrol || !std::isfinite(position.x) || !std::isfinite(position.y)
        || !std::isfinite(position.z)) return false;
    const bool useSelectedValues = patrol->pointSourceMode == ControlPointSourceMode::POINT_OBJECTS
        || (patrol->pointRuntimeMode == ControlPointRuntimeMode::BAKED_STATIC
            && Script::IsInPlayMode());
    if (!useSelectedValues) {
        if (patrol->pointSourceMode != ControlPointSourceMode::INLINE
            || index >= patrol->waypoints.size()) return false;
        patrol->waypoints[index] = position;
        patrol->pointRuntimeMode = ControlPointRuntimeMode::LIVE;
        patrol->started = false;
        return true;
    }
    if (!script || !script->m_scene || !script->m_gameObject) return false;
    const auto snapshot = ResolveScriptPatrolPoints(*script->m_scene,
        script->m_gameObject->GetID(), *patrol);
    if (!snapshot.valid || index >= snapshot.points.size()) return false;
    std::vector<math::Vector3> positions;
    std::vector<float> waitTimes;
    std::vector<float> speeds;
    if (!MaterializePatrolPoints(snapshot, positions, waitTimes, speeds)) return false;
    positions[index] = position;
    patrol->waypoints = std::move(positions);
    patrol->waypointWaitTimes = std::move(waitTimes);
    patrol->waypointSpeeds = std::move(speeds);
    patrol->pointSourceMode = ControlPointSourceMode::INLINE;
    patrol->pointRuntimeMode = ControlPointRuntimeMode::LIVE;
    patrol->currentWaypointId = 0;
    patrol->started = false;
    return true;
}

void ScriptPatrolProxy::Restart(size_t startIndex) const
{
    if (auto* patrol = SelfComponent<NavMeshPatrolComponent>(script)) {
        const size_t pointCount = GetWaypointCount();
        patrol->currentIndex = pointCount == 0 ? 0 : (std::min)(startIndex, pointCount - 1);
        patrol->currentWaypointId = 0;
        patrol->direction = 1;
        patrol->waiting = false;
        patrol->waitTimer = 0.0f;
        patrol->started = false;
    }
}

int ScriptPatrolProxy::GetCurrentIndex() const
{
    const auto* patrol = SelfComponent<NavMeshPatrolComponent>(script);
    return patrol ? static_cast<int>(patrol->currentIndex) : -1;
}

bool ScriptPatrolProxy::IsEnabled() const
{
    const auto* patrol = SelfComponent<NavMeshPatrolComponent>(script);
    return patrol && patrol->enabled;
}

size_t ScriptPatrolProxy::GetWaypointCount() const
{
    const auto* patrol = SelfComponent<NavMeshPatrolComponent>(script);
    if (!patrol || !script || !script->m_scene || !script->m_gameObject) return 0u;
    const auto snapshot = ResolveScriptPatrolPoints(*script->m_scene,
        script->m_gameObject->GetID(), *patrol);
    return snapshot.valid ? snapshot.points.size() : 0u;
}

bool ScriptPatrolProxy::GetWaypoint(size_t index, math::Vector3& position) const
{
    const auto* patrol = SelfComponent<NavMeshPatrolComponent>(script);
    if (!patrol || !script || !script->m_scene || !script->m_gameObject) return false;
    const auto snapshot = ResolveScriptPatrolPoints(*script->m_scene,
        script->m_gameObject->GetID(), *patrol);
    if (!snapshot.valid || index >= snapshot.points.size()) return false;
    position = snapshot.points[index].worldPosition;
    return true;
}

/// @brief ScriptWindProxy
/// @brief 環境風はシーン設定 (SceneEnvironment) になった。スクリプトから見た «風» の語彙は
/// @brief そのままで、触る先だけを差し替えてある。
/// @note この GameObject に FlowField が付いているかは関係しない。環境風は場所を持たない。
/// @note m_scene に触れるのはプロキシ本体だけ (Script の friend)。自由関数に畳めないので
/// @note 各メソッドで 1 行ずつ引く。


} /// @note namespace fbzz::scene
