/// @file    NavMeshPatrolComponent.hpp
/// @brief   NavMesh patrol route authoring and runtime state.
/// @author  Hasegawa Jin
/// @date    2026-06-17
#pragma once

#include <Engine/Scene/ControlPointModes.hpp>
#include <Engine/Scene/ObjectReference.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace fbzz::scene {

inline constexpr std::size_t MAX_NAV_MESH_PATROL_WAYPOINTS = 4096;
inline constexpr std::uint32_t NAV_MESH_PATROL_ADAPTER_VERSION = 1;

/// @brief One ordered object-backed patrol waypoint with its attached movement attributes.
struct NavMeshPatrolWaypointBinding {
    std::uint32_t id = 0;
    ObjectReference target;
    float waitTime = 0.0f;
    float speed = 0.0f;

    /// @brief Reflect a typed TargetPoint reference and attributes as one ordered row.
    void Reflect(IReflector& reflector)
    {
        int reflectedId = id <= 0x7fffffffu ? static_cast<int>(id) : -1;
        reflector.Field("id", reflectedId);
        id = reflectedId > 0 ? static_cast<std::uint32_t>(reflectedId) : 0u;
        reflector.TypedObjectReferenceField("target", target, "TargetPointComponent");
        reflector.Field("waitTime", waitTime);
        reflector.Field("speed", speed);
    }
};

/// @brief Numeric world-space patrol waypoint captured by a static bake.
struct NavMeshPatrolWaypointValue {
    std::uint32_t id = 0;
    math::Vector3 worldPosition = math::Vector3::ZERO;
    float waitTime = 0.0f;
    float speed = 0.0f;
    bool operator==(const NavMeshPatrolWaypointValue&) const = default;
};

/// @brief Reflect ordered point-object rows while retaining one over-limit sentinel row for validation.
inline void ReflectNavMeshPatrolWaypointBindings(IReflector& reflector,
    std::vector<NavMeshPatrolWaypointBinding>& values)
{
    const std::size_t requested = reflector.BeginObjectList("waypointBindings", values.size());
    if (requested != values.size())
        values.resize((std::min)(requested, MAX_NAV_MESH_PATROL_WAYPOINTS + 1));
    for (std::size_t index = 0; index < values.size(); ++index) {
        reflector.BeginObjectElement(index);
        values[index].Reflect(reflector);
        reflector.EndObjectElement();
    }
    const std::size_t removed = reflector.EndObjectList();
    if (removed < values.size()) values.erase(values.begin() + static_cast<std::ptrdiff_t>(removed));
    std::size_t from = 0;
    std::size_t to = 0;
    if (reflector.ObjectListMove(from, to) && from < values.size() && to < values.size() && from != to) {
        NavMeshPatrolWaypointBinding moved = std::move(values[from]);
        values.erase(values.begin() + static_cast<std::ptrdiff_t>(from));
        values.insert(values.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
    }
}

/// @brief Reflect numeric static-bake rows while retaining one over-limit sentinel row for validation.
inline void ReflectNavMeshPatrolWaypointValues(IReflector& reflector,
    std::vector<NavMeshPatrolWaypointValue>& values)
{
    const std::size_t requested = reflector.BeginObjectList("bakedWaypoints", values.size());
    if (requested != values.size())
        values.resize((std::min)(requested, MAX_NAV_MESH_PATROL_WAYPOINTS + 1));
    for (std::size_t index = 0; index < values.size(); ++index) {
        reflector.BeginObjectElement(index);
        int reflectedId = values[index].id <= 0x7fffffffu
            ? static_cast<int>(values[index].id) : -1;
        reflector.Field("id", reflectedId);
        values[index].id = reflectedId > 0 ? static_cast<std::uint32_t>(reflectedId) : 0u;
        reflector.Field("worldPosition", values[index].worldPosition);
        reflector.Field("waitTime", values[index].waitTime);
        reflector.Field("speed", values[index].speed);
        reflector.EndObjectElement();
    }
    const std::size_t removed = reflector.EndObjectList();
    if (removed < values.size()) values.erase(values.begin() + static_cast<std::ptrdiff_t>(removed));
}

/// @brief Authored ordered patrol route and its runtime cursor.
struct NavMeshPatrolComponent {
    /// @brief Patrol direction policy.
    enum class Mode : std::uint8_t {
        LOOP,
        PING_PONG,
    };

    std::vector<math::Vector3> waypoints;
    std::vector<float> waypointWaitTimes;
    std::vector<float> waypointSpeeds;
    std::vector<NavMeshPatrolWaypointBinding> waypointBindings;
    std::vector<NavMeshPatrolWaypointValue> bakedWaypoints;

    ControlPointSourceMode pointSourceMode = ControlPointSourceMode::INLINE;
    ControlPointRuntimeMode pointRuntimeMode = ControlPointRuntimeMode::LIVE;
    ControlPointSourceMode bakedSourceMode = ControlPointSourceMode::INLINE;
    std::uint32_t bakedAdapterVersion = 0;
    std::uint64_t bakedInputSignature = 0;
    bool bakeValid = false;

    /// @note Route progression policy remains live configuration and is independent of numeric point baking.
    Mode mode = Mode::LOOP;
    float waitTime = 0.0f;
    float liveRepathInterval = 0.5f;
    float liveRepathDistance = 0.25f;
    bool enabled = true;

    std::size_t currentIndex = 0;
    std::uint32_t currentWaypointId = 0;
    int direction = 1;
    bool waiting = false;
    float waitTimer = 0.0f;
    bool started = false;
    float liveRepathTimer = 0.0f;
    math::Vector3 lastSubmittedPosition = math::Vector3::ZERO;
    bool hasSubmittedPosition = false;
    std::uint64_t lastDiagnosticSignature = 0;

    /// @brief Return the Inspector component label.
    const char* GetTypeName() const { return "NavMesh Patrol"; }

    /// @brief Reflect settings and typed point bindings; legacy numeric arrays use the Scene codec.
    void Reflect(IReflector& reflector)
    {
        reflector.Field("enabled", enabled);
        reflector.Field("waitTime", waitTime);
        ReflectControlPointSourceMode(reflector, "pointSourceMode", pointSourceMode);
        ReflectControlPointRuntimeMode(reflector, "pointRuntimeMode", pointRuntimeMode);
        reflector.Field("liveRepathInterval", liveRepathInterval);
        reflector.Field("liveRepathDistance", liveRepathDistance);
        if (pointSourceMode == ControlPointSourceMode::POINT_OBJECTS)
            ReflectNavMeshPatrolWaypointBindings(reflector, waypointBindings);
        ReflectNavMeshPatrolWaypointValues(reflector, bakedWaypoints);
        ReflectControlPointSourceMode(reflector, "bakedSourceMode", bakedSourceMode);
        ReflectControlPointVersion(reflector, "bakedAdapterVersion", bakedAdapterVersion);
        ReflectControlPointSignature(reflector, "bakedInputSignature", bakedInputSignature);
        reflector.Field("bakeValid", bakeValid);
    }
};

} /// @note namespace fbzz::scene
