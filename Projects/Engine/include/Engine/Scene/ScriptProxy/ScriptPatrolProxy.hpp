/// @file    ScriptPatrolProxy.hpp
/// @brief   Script から NavMeshPatrolComponent の経路と実行状態を操作するプロキシ。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#pragma once

#include <Math/Vector3.hpp>
#include <cstddef>
#include <cstdint>

namespace fbzz::scene {

class Script;

enum class ScriptPatrolMode : uint8_t {
    LOOP = 0,
    PING_PONG
};

struct ScriptPatrolProxy {
    Script* script = nullptr;

    void SetEnabled(bool enabled) const;
    void SetMode(ScriptPatrolMode mode) const;
    void SetWaitTime(float seconds) const;
    /// @note Clear selects INLINE+LIVE and retains inactive binding metadata and referenced GameObjects.
    void ClearWaypoints() const;
    /// @note Numeric writes select INLINE+LIVE; point-object routes are materialized before appending.
    void AddWaypoint(const math::Vector3& position, float waitTime = -1.0f,
                     float speed = -1.0f) const;
    /// @brief Replace one numeric waypoint in world space.
    /// @return False for an invalid index or unresolved active route; failure leaves source and numeric rows unchanged.
    /// @note Point-object routes are materialized before replacement, retaining other points and attributes.
    bool SetWaypoint(size_t index, const math::Vector3& position) const;
    void Restart(size_t startIndex = 0) const;
    [[nodiscard]] int GetCurrentIndex() const;
    [[nodiscard]] bool IsEnabled() const;
    /// @brief Return the point count from the selected runtime or authoring route.
    [[nodiscard]] size_t GetWaypointCount() const;
    /// @brief Read a waypoint's world position without changing the output on failure.
    /// @return False if the component, route, or index is invalid.
    [[nodiscard]] bool GetWaypoint(size_t index, math::Vector3& position) const;
};

} // namespace fbzz::scene
