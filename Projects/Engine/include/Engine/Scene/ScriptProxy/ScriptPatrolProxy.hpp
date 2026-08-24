// FBZZ Engine
// ScriptPatrolProxy.hpp | fbzz::scene
// Script から NavMeshPatrolComponent の経路と実行状態を操作するプロキシ
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
    void ClearWaypoints() const;
    void AddWaypoint(const math::Vector3& position, float waitTime = -1.0f,
                     float speed = -1.0f) const;
    bool SetWaypoint(size_t index, const math::Vector3& position) const;
    void Restart(size_t startIndex = 0) const;
    [[nodiscard]] int GetCurrentIndex() const;
    [[nodiscard]] bool IsEnabled() const;
    [[nodiscard]] size_t GetWaypointCount() const;
    // index が範囲外なら false を返し、position は変更しない。
    [[nodiscard]] bool GetWaypoint(size_t index, math::Vector3& position) const;
};

} // namespace fbzz::scene
