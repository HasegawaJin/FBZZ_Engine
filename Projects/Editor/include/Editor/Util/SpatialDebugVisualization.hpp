/// @file    SpatialDebugVisualization.hpp
/// @brief   Scene View spatial diagnostics controls shared by UI, operators, and capture requests.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <string>

namespace fbzz::editor {

enum class SpatialDebugTarget { RENDER, PHYSICS };

/// @brief Composable display settings; disabled controls request no snapshots.
/// @note Draw budget counts value commands, including pair connecting lines.
struct SpatialDebugTargetSettings {
    bool enabled = false;
    bool hierarchy = true;
    bool proxyBounds = false;
    bool candidatePairs = false;
    int minDepth = 0;
    int maxDepth = 32;
    int maxDrawCommands = 512;
    bool selectedOnly = false;
    bool depthTest = false;

    void Clamp()
    {
        minDepth = std::clamp(minDepth, 0, 64);
        maxDepth = std::clamp(maxDepth, minDepth, 64);
        maxDrawCommands = std::clamp(maxDrawCommands, 1, 4096);
    }

    [[nodiscard]] bool CapturesHierarchy() const { return enabled && hierarchy; }
    [[nodiscard]] bool CapturesProxyBounds() const { return enabled && proxyBounds; }
    [[nodiscard]] bool CapturesCandidatePairs() const { return enabled && candidatePairs; }
    [[nodiscard]] bool AnyCapture() const
    {
        return CapturesHierarchy() || CapturesProxyBounds() || CapturesCandidatePairs();
    }
};

/// @brief Editor-only state; the target selector does not disable the other target.
/// @note Status text and traversal availability are replaced by the owner bridge after snapshot validation.
struct SpatialDebugVisualization {
    SpatialDebugTarget target = SpatialDebugTarget::PHYSICS;
    SpatialDebugTargetSettings render;
    SpatialDebugTargetSettings physics;
    std::string renderStatus = "OFF";
    std::string physicsStatus = "OFF";
    bool renderTraversalStatusAvailable = false;
    bool physicsTraversalStatusAvailable = false;
    uint64_t captureSceneGeneration = 0;
    uint64_t physicsRequestRevision = 0;
    bool captureRuntimeActive = false;
    std::size_t renderDrawCommands = 0;
    std::size_t physicsDrawCommands = 0;

    [[nodiscard]] SpatialDebugTargetSettings& Selected()
    {
        return target == SpatialDebugTarget::RENDER ? render : physics;
    }
};

} /// @note namespace fbzz::editor
