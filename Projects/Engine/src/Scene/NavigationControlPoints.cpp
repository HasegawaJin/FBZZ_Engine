/// @file    NavigationControlPoints.cpp
/// @brief   Resolve navigation control-point authoring and static bake inputs.
/// @author  Hasegawa Jin
/// @date    2026-10-08
#include <Engine/Scene/Components/NavMeshOffMeshLinkComponent.hpp>
#include <Engine/Scene/Components/NavMeshPatrolComponent.hpp>
#include <Engine/Scene/NavigationControlPoints.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>

namespace fbzz::scene {
namespace {

constexpr std::uint64_t FNV_OFFSET = 14695981039346656037ull;
constexpr std::uint64_t FNV_PRIME = 1099511628211ull;
constexpr std::uint32_t OFF_MESH_LINK_ADAPTER_VERSION = 1;

void HashBytes(std::uint64_t& hash, const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= FNV_PRIME;
    }
}

template<typename T>
void HashValue(std::uint64_t& hash, const T& value)
{
    HashBytes(hash, &value, sizeof(value));
}

void HashString(std::uint64_t& hash, const std::string& value)
{
    HashValue(hash, static_cast<std::uint64_t>(value.size()));
    HashBytes(hash, value.data(), value.size());
}

void HashFloat(std::uint64_t& hash, float value)
{
    HashValue(hash, std::bit_cast<std::uint32_t>(value));
}

void HashVector(std::uint64_t& hash, const math::Vector3& value)
{
    HashFloat(hash, value.x);
    HashFloat(hash, value.y);
    HashFloat(hash, value.z);
}

void HashMode(std::uint64_t& hash, ControlPointSourceMode sourceMode,
    ControlPointRuntimeMode runtimeMode)
{
    HashValue(hash, sourceMode);
    HashValue(hash, runtimeMode);
    HashValue(hash, NAV_MESH_PATROL_ADAPTER_VERSION);
}

void HashPatrolValue(std::uint64_t& hash, const NavMeshPatrolPointValue& point)
{
    HashValue(hash, point.stableId);
    HashVector(hash, point.worldPosition);
    HashFloat(hash, point.waitTime);
    HashFloat(hash, point.speed);
}

bool IsFinite(const math::Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool IsFinite(const NavMeshPatrolPointValue& value)
{
    return value.stableId != 0 && IsFinite(value.worldPosition)
        && std::isfinite(value.waitTime) && std::isfinite(value.speed);
}

bool IsKnownSourceMode(ControlPointSourceMode mode)
{
    return mode == ControlPointSourceMode::INLINE
        || mode == ControlPointSourceMode::POINT_OBJECTS;
}

bool IsKnownRuntimeMode(ControlPointRuntimeMode mode)
{
    return mode == ControlPointRuntimeMode::LIVE
        || mode == ControlPointRuntimeMode::BAKED_STATIC;
}

std::uint64_t RawPatrolSignature(const NavMeshPatrolComponent& patrol,
    ControlPointRuntimeMode runtimeMode)
{
    std::uint64_t hash = FNV_OFFSET;
    HashMode(hash, patrol.pointSourceMode, runtimeMode);
    HashValue(hash, patrol.mode);
    HashFloat(hash, patrol.waitTime);
    if (patrol.pointSourceMode == ControlPointSourceMode::INLINE) {
        HashValue(hash, static_cast<std::uint64_t>(patrol.waypoints.size()));
        const std::size_t boundedCount = (std::min)(patrol.waypoints.size(),
            MAX_NAV_MESH_PATROL_WAYPOINTS + 1);
        for (std::size_t index = 0; index < boundedCount; ++index) {
            const std::uint32_t stableId = static_cast<std::uint32_t>(index + 1);
            const float wait = index < patrol.waypointWaitTimes.size()
                ? patrol.waypointWaitTimes[index] : patrol.waitTime;
            const float speed = index < patrol.waypointSpeeds.size()
                ? patrol.waypointSpeeds[index] : 0.0f;
            HashValue(hash, stableId);
            HashVector(hash, patrol.waypoints[index]);
            HashFloat(hash, wait);
            HashFloat(hash, speed);
        }
    } else if (patrol.pointSourceMode == ControlPointSourceMode::POINT_OBJECTS) {
        HashValue(hash, static_cast<std::uint64_t>(patrol.waypointBindings.size()));
        const std::size_t boundedCount = (std::min)(patrol.waypointBindings.size(),
            MAX_NAV_MESH_PATROL_WAYPOINTS + 1);
        for (std::size_t index = 0; index < boundedCount; ++index) {
            const NavMeshPatrolWaypointBinding& binding = patrol.waypointBindings[index];
            HashValue(hash, binding.id);
            HashString(hash, binding.target.guid);
            HashFloat(hash, binding.waitTime);
            HashFloat(hash, binding.speed);
        }
    } else {
        HashValue(hash, static_cast<std::uint64_t>(patrol.waypoints.size()));
        HashValue(hash, static_cast<std::uint64_t>(patrol.waypointBindings.size()));
        const std::size_t numericCount = (std::min)(patrol.waypoints.size(),
            MAX_NAV_MESH_PATROL_WAYPOINTS + 1);
        for (std::size_t index = 0; index < numericCount; ++index)
            HashVector(hash, patrol.waypoints[index]);
        const std::size_t bindingCount = (std::min)(patrol.waypointBindings.size(),
            MAX_NAV_MESH_PATROL_WAYPOINTS + 1);
        for (std::size_t index = 0; index < bindingCount; ++index) {
            const NavMeshPatrolWaypointBinding& binding = patrol.waypointBindings[index];
            HashValue(hash, binding.id);
            HashString(hash, binding.target.guid);
            HashFloat(hash, binding.waitTime);
            HashFloat(hash, binding.speed);
        }
    }
    return hash == 0 ? 1 : hash;
}

bool ValidatePatrolValues(const std::vector<NavMeshPatrolPointValue>& values)
{
    if (values.size() > MAX_NAV_MESH_PATROL_WAYPOINTS) return false;
    std::unordered_set<std::uint32_t> ids;
    ids.reserve(values.size());
    for (const NavMeshPatrolPointValue& value : values) {
        if (!IsFinite(value) || !ids.insert(value.stableId).second) return false;
    }
    return true;
}

bool SameBakedValues(const std::vector<NavMeshPatrolPointValue>& current,
    const std::vector<NavMeshPatrolWaypointValue>& baked)
{
    if (current.size() != baked.size()) return false;
    for (std::size_t index = 0; index < current.size(); ++index) {
        const NavMeshPatrolPointValue& lhs = current[index];
        const NavMeshPatrolWaypointValue& rhs = baked[index];
        if (lhs.stableId != rhs.id || lhs.worldPosition != rhs.worldPosition
            || lhs.waitTime != rhs.waitTime || lhs.speed != rhs.speed)
            return false;
    }
    return true;
}

void HashResolvedPoints(std::uint64_t& hash,
    const std::vector<NavMeshPatrolPointValue>& points)
{
    HashValue(hash, static_cast<std::uint64_t>(points.size()));
    for (const NavMeshPatrolPointValue& point : points) HashPatrolValue(hash, point);
    if (hash == 0) hash = 1;
}

std::uint64_t CanonicalPatrolSignature(const NavMeshPatrolComponent& patrol,
    ControlPointRuntimeMode runtimeMode,
    const std::vector<NavMeshPatrolPointValue>& points)
{
    std::uint64_t hash = FNV_OFFSET;
    HashMode(hash, patrol.pointSourceMode, runtimeMode);
    HashResolvedPoints(hash, points);
    return hash == 0 ? 1 : hash;
}

void HashOffMeshRaw(std::uint64_t& hash, const NavMeshOffMeshLinkComponent& link)
{
    HashValue(hash, link.pointSourceMode);
    HashValue(hash, OFF_MESH_LINK_ADAPTER_VERSION);
    if (link.pointSourceMode == ControlPointSourceMode::INLINE) {
        HashVector(hash, link.startPoint);
        HashVector(hash, link.endPoint);
    } else if (link.pointSourceMode == ControlPointSourceMode::POINT_OBJECTS) {
        HashString(hash, link.startTarget.guid);
        HashString(hash, link.endTarget.guid);
    } else {
        HashVector(hash, link.startPoint);
        HashVector(hash, link.endPoint);
        HashString(hash, link.startTarget.guid);
        HashString(hash, link.endTarget.guid);
    }
}

} /// @note namespace

NavMeshPatrolPointSnapshot ResolveNavMeshPatrolPointAuthoring(
    Scene& scene, EntityID ownerId, const NavMeshPatrolComponent& patrol,
    ControlPointRuntimeMode signatureRuntimeMode)
{
    NavMeshPatrolPointSnapshot snapshot;
    snapshot.inputSignature = RawPatrolSignature(patrol, signatureRuntimeMode);
    if (!IsKnownRuntimeMode(signatureRuntimeMode)) {
        snapshot.status = NavigationControlPointStatus::INVALID_RUNTIME_MODE;
        return snapshot;
    }
    if (!IsKnownSourceMode(patrol.pointSourceMode)) {
        snapshot.status = NavigationControlPointStatus::INVALID_SOURCE_MODE;
        return snapshot;
    }
    if (patrol.mode != NavMeshPatrolComponent::Mode::LOOP
        && patrol.mode != NavMeshPatrolComponent::Mode::PING_PONG) {
        snapshot.status = NavigationControlPointStatus::INVALID_NUMERIC_DATA;
        return snapshot;
    }

    std::uint64_t resolverSignature = 0;
    if (patrol.pointSourceMode == ControlPointSourceMode::INLINE) {
        if (patrol.waypoints.size() > MAX_NAV_MESH_PATROL_WAYPOINTS) {
            snapshot.status = NavigationControlPointStatus::TOO_MANY_POINTS;
            return snapshot;
        }
        snapshot.points.reserve(patrol.waypoints.size());
        for (std::size_t index = 0; index < patrol.waypoints.size(); ++index) {
            const std::uint32_t stableId = static_cast<std::uint32_t>(index + 1);
            const float wait = index < patrol.waypointWaitTimes.size()
                ? patrol.waypointWaitTimes[index] : patrol.waitTime;
            const float speed = index < patrol.waypointSpeeds.size()
                ? patrol.waypointSpeeds[index] : 0.0f;
            snapshot.points.push_back({stableId, patrol.waypoints[index], wait, speed});
        }
    } else {
        if (patrol.waypointBindings.size() > MAX_NAV_MESH_PATROL_WAYPOINTS) {
            snapshot.status = NavigationControlPointStatus::TOO_MANY_POINTS;
            snapshot.resolveStatus = ControlPointResolveStatus::TOO_MANY_BINDINGS;
            return snapshot;
        }
        std::vector<ControlPointBinding> bindings;
        bindings.reserve(patrol.waypointBindings.size());
        for (const NavMeshPatrolWaypointBinding& binding : patrol.waypointBindings)
            bindings.push_back({binding.id, &binding.target});
        std::vector<ResolvedControlPoint> resolved;
        if (!ResolveControlPointBindings(scene, ownerId, bindings, true,
                resolved, snapshot.resolveStatus, resolverSignature)) {
            snapshot.status = NavigationControlPointStatus::RESOLUTION_FAILED;
            HashValue(snapshot.inputSignature, snapshot.resolveStatus);
            if (snapshot.inputSignature == 0) snapshot.inputSignature = 1;
            return snapshot;
        }
        if (signatureRuntimeMode == ControlPointRuntimeMode::LIVE) {
            GameObject* owner = scene.GetGameObject(ownerId);
            if (!owner) {
                snapshot.status = NavigationControlPointStatus::RESOLUTION_FAILED;
                snapshot.resolveStatus = ControlPointResolveStatus::INVALID_OWNER;
                HashValue(snapshot.inputSignature, snapshot.resolveStatus);
                return snapshot;
            }
            for (const ResolvedControlPoint& point : resolved) {
                GameObject* target = scene.GetGameObject(point.targetId);
                if (target && (target == owner || target->IsDescendantOf(*owner))) {
                    snapshot.status = NavigationControlPointStatus::SELF_REFERENTIAL_TARGET;
                    HashValue(snapshot.inputSignature, snapshot.status);
                    if (snapshot.inputSignature == 0) snapshot.inputSignature = 1;
                    return snapshot;
                }
            }
        }
        snapshot.points.reserve(resolved.size());
        for (std::size_t index = 0; index < resolved.size(); ++index) {
            const NavMeshPatrolWaypointBinding& binding = patrol.waypointBindings[index];
            snapshot.points.push_back({resolved[index].stableId,
                resolved[index].worldPosition, binding.waitTime, binding.speed});
        }
    }

    const bool needsGlobalWait = patrol.pointSourceMode == ControlPointSourceMode::INLINE
        && patrol.waypointWaitTimes.size() < patrol.waypoints.size();
    const bool needsLiveRepath = patrol.pointSourceMode == ControlPointSourceMode::POINT_OBJECTS
        && signatureRuntimeMode == ControlPointRuntimeMode::LIVE;
    if ((needsGlobalWait && !std::isfinite(patrol.waitTime))
        || (needsLiveRepath && (!std::isfinite(patrol.liveRepathInterval)
            || !std::isfinite(patrol.liveRepathDistance)))
        || !ValidatePatrolValues(snapshot.points)) {
        snapshot.status = NavigationControlPointStatus::INVALID_NUMERIC_DATA;
        HashResolvedPoints(snapshot.inputSignature, snapshot.points);
        return snapshot;
    }
    snapshot.inputSignature = CanonicalPatrolSignature(
        patrol, signatureRuntimeMode, snapshot.points);
    snapshot.status = NavigationControlPointStatus::SUCCESS;
    snapshot.resolveStatus = ControlPointResolveStatus::SUCCESS;
    snapshot.valid = true;
    return snapshot;
}

NavMeshPatrolPointSnapshot ResolveNavMeshPatrolPoints(
    Scene& scene, EntityID ownerId, const NavMeshPatrolComponent& patrol,
    ControlPointReadContext context)
{
    if (patrol.pointRuntimeMode == ControlPointRuntimeMode::LIVE)
        return ResolveNavMeshPatrolPointAuthoring(scene, ownerId, patrol,
            ControlPointRuntimeMode::LIVE);

    NavMeshPatrolPointSnapshot snapshot;
    snapshot.inputSignature = patrol.bakedInputSignature;
    if (!IsKnownRuntimeMode(patrol.pointRuntimeMode)) {
        snapshot.status = NavigationControlPointStatus::INVALID_RUNTIME_MODE;
        snapshot.inputSignature = RawPatrolSignature(patrol, patrol.pointRuntimeMode);
        return snapshot;
    }
    if (patrol.mode != NavMeshPatrolComponent::Mode::LOOP
        && patrol.mode != NavMeshPatrolComponent::Mode::PING_PONG) {
        std::uint64_t hash = FNV_OFFSET;
        HashValue(hash, patrol.bakedInputSignature);
        HashValue(hash, patrol.mode);
        snapshot.inputSignature = hash == 0 ? 1 : hash;
        snapshot.status = NavigationControlPointStatus::INVALID_NUMERIC_DATA;
        return snapshot;
    }
    if (context == ControlPointReadContext::AUTHORING) {
        snapshot = ResolveNavMeshPatrolPointAuthoring(scene, ownerId, patrol,
            ControlPointRuntimeMode::BAKED_STATIC);
        snapshot.bakeStale = !snapshot.valid
            || snapshot.inputSignature != patrol.bakedInputSignature
            || patrol.bakedSourceMode != patrol.pointSourceMode
            || !SameBakedValues(snapshot.points, patrol.bakedWaypoints);
    }
    if (!patrol.bakeValid || patrol.bakedInputSignature == 0
        || patrol.bakedAdapterVersion != NAV_MESH_PATROL_ADAPTER_VERSION
        || !IsKnownSourceMode(patrol.bakedSourceMode)
        || patrol.bakedSourceMode != patrol.pointSourceMode) {
        snapshot.points.clear();
        snapshot.status = patrol.bakeValid
            ? NavigationControlPointStatus::STALE_BAKE
            : NavigationControlPointStatus::MISSING_BAKE;
        snapshot.bakeStale = true;
        snapshot.valid = false;
        return snapshot;
    }

    std::vector<NavMeshPatrolPointValue> baked;
    if (patrol.bakedWaypoints.size() > MAX_NAV_MESH_PATROL_WAYPOINTS) {
        snapshot.status = NavigationControlPointStatus::TOO_MANY_POINTS;
        snapshot.valid = false;
        return snapshot;
    }
    baked.reserve(patrol.bakedWaypoints.size());
    for (const NavMeshPatrolWaypointValue& point : patrol.bakedWaypoints)
        baked.push_back({point.id, point.worldPosition, point.waitTime, point.speed});
    if (!ValidatePatrolValues(baked)) {
        snapshot.points.clear();
        snapshot.status = NavigationControlPointStatus::INVALID_NUMERIC_DATA;
        snapshot.valid = false;
        return snapshot;
    }
    /// @note Authoring reads retain source values after the saved bake has passed its existing validation.
    if (context == ControlPointReadContext::AUTHORING) {
        if (snapshot.valid && snapshot.bakeStale)
            snapshot.status = NavigationControlPointStatus::STALE_BAKE;
        return snapshot;
    }
    snapshot.points = std::move(baked);
    snapshot.valid = true;
    snapshot.status = snapshot.bakeStale
        ? NavigationControlPointStatus::STALE_BAKE
        : NavigationControlPointStatus::SUCCESS;
    return snapshot;
}

NavMeshOffMeshLinkSnapshot ResolveNavMeshOffMeshLinkEndpoints(
    Scene& scene, EntityID ownerId, const NavMeshOffMeshLinkComponent& link)
{
    NavMeshOffMeshLinkSnapshot snapshot;
    snapshot.inputSignature = FNV_OFFSET;
    HashOffMeshRaw(snapshot.inputSignature, link);
    if (!IsKnownSourceMode(link.pointSourceMode)) {
        snapshot.status = NavigationControlPointStatus::INVALID_SOURCE_MODE;
        return snapshot;
    }
    if (!std::isfinite(link.traversalTime)) {
        snapshot.status = NavigationControlPointStatus::INVALID_NUMERIC_DATA;
        return snapshot;
    }
    if (link.pointSourceMode == ControlPointSourceMode::INLINE) {
        if (!IsFinite(link.startPoint) || !IsFinite(link.endPoint)) {
            snapshot.status = NavigationControlPointStatus::INVALID_NUMERIC_DATA;
            return snapshot;
        }
        snapshot.startPoint = link.startPoint;
        snapshot.endPoint = link.endPoint;
    } else {
        const std::array<ControlPointBinding, 2> bindings{ {
            {1u, &link.startTarget},
            {2u, &link.endTarget},
        } };
        std::vector<ResolvedControlPoint> resolved;
        std::uint64_t resolverSignature = 0;
        if (!ResolveControlPointBindings(scene, ownerId, bindings, true,
                resolved, snapshot.resolveStatus, resolverSignature)) {
            snapshot.status = NavigationControlPointStatus::RESOLUTION_FAILED;
            HashValue(snapshot.inputSignature, snapshot.resolveStatus);
            if (snapshot.inputSignature == 0) snapshot.inputSignature = 1;
            return snapshot;
        }
        snapshot.startPoint = resolved[0].worldPosition;
        snapshot.endPoint = resolved[1].worldPosition;
        HashValue(snapshot.inputSignature, resolverSignature);
    }
    HashVector(snapshot.inputSignature, snapshot.startPoint);
    HashVector(snapshot.inputSignature, snapshot.endPoint);
    if (snapshot.inputSignature == 0) snapshot.inputSignature = 1;
    snapshot.status = NavigationControlPointStatus::SUCCESS;
    snapshot.resolveStatus = ControlPointResolveStatus::SUCCESS;
    snapshot.valid = true;
    return snapshot;
}

bool ValidateStaticNavMeshPatrolBakes(Scene& scene)
{
    FlushWorldTransforms(scene);
    bool valid = true;
    for (const EntityID id : scene.GetEntities<NavMeshPatrolComponent>()) {
        const NavMeshPatrolComponent* patrol = scene.GetComponent<NavMeshPatrolComponent>(id);
        if (!patrol || patrol->pointRuntimeMode == ControlPointRuntimeMode::LIVE) continue;
        const NavMeshPatrolPointSnapshot snapshot = ResolveNavMeshPatrolPoints(
            scene, id, *patrol, ControlPointReadContext::AUTHORING);
        if (snapshot.valid && !snapshot.bakeStale
            && snapshot.status == NavigationControlPointStatus::SUCCESS) continue;
        FBZZ_LOG_ERROR("NavigationControlPoints: Patrol static bake is not ready on entity %u:%u (status=%u, resolve=%u).",
            id.index, id.generation, static_cast<unsigned>(snapshot.status),
            static_cast<unsigned>(snapshot.resolveStatus));
        valid = false;
    }
    return valid;
}

} /// @note namespace fbzz::scene
