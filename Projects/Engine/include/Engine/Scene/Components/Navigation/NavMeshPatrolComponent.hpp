/// @file    NavMeshPatrolComponent.hpp
/// @brief   NavMesh 巡回経路の設定と実行時状態。
/// @author  Hasegawa Jin
/// @date    2026-06-17
#pragma once

#include <Engine/Scene/ControlPoints/ControlPointModes.hpp>
#include <Engine/Scene/References/ObjectReference.hpp>
#include <Engine/Reflection/Reflection.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace fbzz::scene {

inline constexpr std::size_t MAX_NAV_MESH_PATROL_WAYPOINTS = 4096;
inline constexpr std::uint32_t NAV_MESH_PATROL_ADAPTER_VERSION = 1;

/// @brief 対象オブジェクトを参照する巡回点と、その点に付随する移動設定。
struct NavMeshPatrolWaypointBinding {
    std::uint32_t id = 0;
    ObjectReference target;
    float waitTime = 0.0f;
    float speed = 0.0f;

    /// @brief TargetPoint 型参照と設定を一行にまとめて公開する。
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

/// @brief 静的ベイクで取得したワールド座標の巡回点。
struct NavMeshPatrolWaypointValue {
    std::uint32_t id = 0;
    math::Vector3 worldPosition = math::Vector3::ZERO;
    float waitTime = 0.0f;
    float speed = 0.0f;
    bool operator==(const NavMeshPatrolWaypointValue&) const = default;
};

/// @brief 上限超過を検証できるよう余分な一行を保持し、対象オブジェクトの参照を順序通りに公開する。
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

/// @brief 上限超過を検証できるよう余分な一行を保持し、静的ベイクの数値点を公開する。
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

/// @brief 設定された巡回経路と、実行時に選択中の点。
struct NavMeshPatrolComponent {
    /// @brief 巡回方向の方針。
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

    /// @note 巡回順序の方針は実行時に参照し、点座標のベイクとは独立する。
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

    /// @brief Inspector 上のコンポーネント名を返す。
    const char* GetTypeName() const { return "NavMesh Patrol"; }

    /// @brief 設定と型付きの点参照を公開する。従来の数値配列はシーンのコーデックで扱う。
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

}
