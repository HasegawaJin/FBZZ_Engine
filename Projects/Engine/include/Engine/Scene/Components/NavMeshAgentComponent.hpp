/// @file    NavMeshAgentComponent.hpp
/// @brief   NavMesh agent movement settings, target reference, and path state.
/// @author  Hasegawa Jin
/// @date    2026-06-17
#pragma once

#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fbzz::scene {

/// @brief Interpolation state while an agent traverses an Off-Mesh Link.
struct NavMeshLinkTraversal {
    math::Vector3 startPos = math::Vector3::ZERO;
    math::Vector3 endPos = math::Vector3::ZERO;
    float totalTime = 0.3f;
    float elapsed = 0.0f;
    bool active = false;
};

/// @brief Current NavMesh agent movement state.
enum class NavMeshAgentState : std::uint8_t {
    IDLE,
    MOVING,
    TRAVERSING_LINK,
};

/// @brief Navigation tuning, persistent target, and runtime path-following state.
struct NavMeshAgentComponent {
    float radius = 0.4f;
    float maxSpeed = 3.5f;
    float acceleration = 8.0f;
    float angularSpeedDeg = 360.0f;
    float stoppingDistance = 0.1f;
    int avoidancePriority = 0;
    int agentTypeId = 0;
    bool snapToNavMesh = false;
    bool updatePosition = true;
    bool updateRotation = true;
    bool autoBraking = true;
    int areaMask = -1;
    bool enabled = true;

    /// @name Runtime movement state
    /// @{
    bool hasDestination = false;
    math::Vector3 velocity = math::Vector3::ZERO;
    math::Vector3 destination = math::Vector3::ZERO;
    std::vector<math::Vector3> path;
    std::vector<bool> pathLinkFlags;
    std::vector<float> pathLinkTimes;
    std::size_t currentWaypoint = 0;
    NavMeshAgentState state = NavMeshAgentState::IDLE;
    float currentSpeed = 0.0f;
    NavMeshLinkTraversal linkTraversal;
    bool isStopped = false;
    bool destinationReached = false;
    float remainingDistance = 0.0f;
    /// @}

    /// @name Target follow state
    /// @{
    /// @note Assigned missing GUIDs remain assigned and stop the agent instead of enabling fixed-route fallback.
    EntityRef target{};
    float repathInterval = 0.5f;
    float repathTimer = 0.0f;
    math::Vector3 lastTargetPos = math::Vector3::ZERO;
    std::uint64_t lastTargetDiagnosticSignature = 0;
    /// @}

    /// @name Stuck detection state
    /// @{
    math::Vector3 lastStuckCheckPos = math::Vector3::ZERO;
    float stuckTimer = 0.0f;
    bool isStuck = false;
    /// @}

    /// @brief Report whether a path is available.
    [[nodiscard]] bool HasPath() const { return !path.empty(); }

    /// @brief Return the Inspector component label.
    [[nodiscard]] const char* GetTypeName() const { return "NavMesh Agent"; }

    /// @brief Reflect persistent agent settings and its target reference.
    void Reflect(IReflector& reflector)
    {
        reflector.Field("enabled", enabled);
        reflector.Field("radius", radius);
        reflector.Field("maxSpeed", maxSpeed);
        reflector.Field("acceleration", acceleration);
        reflector.Field("angularSpeedDeg", angularSpeedDeg);
        reflector.Field("stoppingDistance", stoppingDistance);
        reflector.Field("avoidancePriority", avoidancePriority);
        reflector.Field("agentTypeId", agentTypeId);
        reflector.Field("snapToNavMesh", snapToNavMesh);
        reflector.Field("updatePosition", updatePosition);
        reflector.Field("updateRotation", updateRotation);
        reflector.Field("autoBraking", autoBraking);
        reflector.Field("areaMask", areaMask);
        reflector.Field("target", target);
        reflector.Field("repathInterval", repathInterval);
    }

    /// @brief Set a fixed world destination and clear target-follow state.
    /// @param worldPosition Destination in world coordinates.
    void SetDestination(const math::Vector3& worldPosition)
    {
        destination = worldPosition;
        hasDestination = true;
        destinationReached = false;
        isStopped = false;
        target = EntityRef{};
        lastTargetDiagnosticSignature = 0;
        path.clear();
        pathLinkFlags.clear();
        pathLinkTimes.clear();
        linkTraversal.active = false;
        currentWaypoint = 0;
        state = NavMeshAgentState::MOVING;
    }

    /// @brief Track an EntityID target using the Scene-local compatibility path.
    /// @param entity Target entity; its current Scene GUID is attached during resolution.
    /// @param interval Minimum time between automatic path recalculations in seconds.
    void SetTarget(EntityID entity, float interval = 0.5f)
    {
        target.guid.clear();
        target.id = entity;
        target.sceneGeneration = 0;
        lastTargetDiagnosticSignature = 0;
        repathInterval = interval > 0.05f ? interval : 0.05f;
        repathTimer = 0.0f;
    }

    /// @brief Clear target GUID and its Scene-local cache.
    void ClearTarget()
    {
        target = EntityRef{};
        lastTargetDiagnosticSignature = 0;
    }

    /// @brief Drop the current route and stop movement.
    void Stop()
    {
        hasDestination = false;
        target = EntityRef{};
        lastTargetDiagnosticSignature = 0;
        path.clear();
        pathLinkFlags.clear();
        pathLinkTimes.clear();
        linkTraversal.active = false;
        currentWaypoint = 0;
        currentSpeed = 0.0f;
        remainingDistance = 0.0f;
        isStopped = false;
        state = NavMeshAgentState::IDLE;
    }
};

} /// @note namespace fbzz::scene
