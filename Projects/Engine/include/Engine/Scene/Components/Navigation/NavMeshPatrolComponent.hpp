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
    std::uint32_t id = 0; ///< 並べ替えや参照の再解決後も同じ経由点を識別する番号。
    ObjectReference target; ///< 経由点のワールド位置を与えるシーン上の対象。
    float waitTime = 0.0f; ///< 対象の経由点で待機する時間 [秒]。
    float speed = 0.0f; ///< 対象の経由点に向かうときの速度指定 [m/s]。

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
    std::uint32_t id = 0; ///< ベイク前の経由点に対応する安定した識別番号。
    math::Vector3 worldPosition = math::Vector3::ZERO; ///< ベイク時に確定した経由点のワールド位置 [m]。
    float waitTime = 0.0f; ///< ベイクで確定した経由点での待機時間 [秒]。
    float speed = 0.0f; ///< ベイクで確定した経由点への移動速度の指定 [m/s]。
    bool operator==(const NavMeshPatrolWaypointValue&) const = default;
};

/// @brief 上限超過を検証できるよう余分な一行を保持し、対象オブジェクトの参照を順序通りに公開する。
inline void ReflectNavMeshPatrolWaypointBindings(IReflector& reflector,
    std::vector<NavMeshPatrolWaypointBinding>& values)
{
    const std::size_t requested = reflector.BeginObjectList("waypointBindings", values.size());
    if (requested != values.size()) {
        values.resize((std::min)(requested, MAX_NAV_MESH_PATROL_WAYPOINTS + 1));
    }
    for (std::size_t index = 0; index < values.size(); ++index) {
        reflector.BeginObjectElement(index);
        values[index].Reflect(reflector);
        reflector.EndObjectElement();
    }
    const std::size_t removed = reflector.EndObjectList();
    if (removed < values.size()) {
        values.erase(values.begin() + static_cast<std::ptrdiff_t>(removed));
    }
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
    if (requested != values.size()) {
        values.resize((std::min)(requested, MAX_NAV_MESH_PATROL_WAYPOINTS + 1));
    }
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
    if (removed < values.size()) {
        values.erase(values.begin() + static_cast<std::ptrdiff_t>(removed));
    }
}

/// @brief 設定された巡回経路と、実行時に選択中の点。
struct NavMeshPatrolComponent {
    /// @brief 巡回方向の方針。
    enum class Mode : std::uint8_t {
        LOOP,
        PING_PONG,
    };

    std::vector<math::Vector3> waypoints; ///< インライン指定の巡回経由点のワールド座標 [m]。巡回する順に並べる。
    std::vector<float> waypointWaitTimes; ///< waypoints と同じ添字で指定する各経由点の待機時間 [秒]。
    std::vector<float> waypointSpeeds; ///< waypoints と同じ添字で指定する各経由点への速度 [m/s]。
    std::vector<NavMeshPatrolWaypointBinding> waypointBindings; ///< シーン上の参照で指定する経由点を巡回順に並べた配列。
    std::vector<NavMeshPatrolWaypointValue> bakedWaypoints; ///< ベイク時に解決した経由点の位置と動作設定のスナップショット。

    ControlPointSourceMode pointSourceMode = ControlPointSourceMode::INLINE; ///< 巡回点をインライン座標から取るか、シーン参照から取るか。
    ControlPointRuntimeMode pointRuntimeMode = ControlPointRuntimeMode::LIVE; ///< 実行時に経由点をその場で解決するか、ベイク結果を使うか。
    ControlPointSourceMode bakedSourceMode = ControlPointSourceMode::INLINE; ///< 現在保持するベイク結果を作成したときの経由点の指定方式。
    std::uint32_t bakedAdapterVersion = 0; ///< ベイク結果を作成した経由点アダプターの版番号。
    std::uint64_t bakedInputSignature = 0; ///< ベイク結果が現在の経由点設定と一致するかを判定する入力の署名。
    bool bakeValid = false; ///< 実行時に使えるベイク済みの経由点を保持しているかどうか。

    Mode mode = Mode::LOOP; ///< 巡回順序の方針は実行時に参照し、点座標のベイクとは独立する。
    float waitTime = 0.0f; ///< 経由点単位の指定がないときの既定の待機時間 [秒]。
    float liveRepathInterval = 0.5f; ///< 移動する参照経由点に対して再探索する最小時間間隔 [秒]。
    float liveRepathDistance = 0.25f; ///< 移動する参照経由点への経路を更新する距離のしきい値 [m]。
    bool enabled = true; ///< このコンポーネントの巡回を進行するかどうか。

    std::size_t currentIndex = 0; ///< 現在向かっている経由点の、選択中の経由点列内の添字。
    std::uint32_t currentWaypointId = 0; ///< 参照先や配列の変化後に現在の経由点を探し直す識別番号。
    int direction = 1; ///< 経由点の添字を進める方向。1 は順送り、-1 は逆送り。
    bool waiting = false; ///< 到着した経由点で現在待機しているかどうか。
    float waitTimer = 0.0f; ///< 経由点での待機を進めるタイマー [秒]。
    bool started = false; ///< 現在の巡回で最初の目的地を設定済みかどうか。
    float liveRepathTimer = 0.0f; ///< 動く経由点を追従するための経路再計算タイマー [秒]。
    math::Vector3 lastSubmittedPosition = math::Vector3::ZERO; ///< エージェントへ最後に設定した目的地のワールド位置 [m]。
    bool hasSubmittedPosition = false; ///< 比較に使える前回の目的地を保持しているかどうか。
    std::uint64_t lastDiagnosticSignature = 0; ///< 同じ経由点の診断を繰り返さないための直近の署名。

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
        if (pointSourceMode == ControlPointSourceMode::POINT_OBJECTS) {
            ReflectNavMeshPatrolWaypointBindings(reflector, waypointBindings);
        }
        ReflectNavMeshPatrolWaypointValues(reflector, bakedWaypoints);
        ReflectControlPointSourceMode(reflector, "bakedSourceMode", bakedSourceMode);
        ReflectControlPointVersion(reflector, "bakedAdapterVersion", bakedAdapterVersion);
        ReflectControlPointSignature(reflector, "bakedInputSignature", bakedInputSignature);
        reflector.Field("bakeValid", bakeValid);
    }
};

}
