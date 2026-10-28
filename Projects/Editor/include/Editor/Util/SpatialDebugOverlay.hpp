/// @file    SpatialDebugOverlay.hpp
/// @brief   Validated immutable spatial snapshots to bounded Scene View value commands.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#pragma once
#include <Editor/Util/SpatialDebugVisualization.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::renderer { struct RenderBvhDebugSnapshot; }
namespace fbzz::scene { class Scene; struct ScriptDebugDrawCommand; struct PhysicsSpatialDebugSnapshot; }
namespace fbzz::editor {
struct EditorContext;

struct SpatialDebugOverlayContext {
    uint64_t sceneGeneration = 0;
    uint64_t currentResourceFrameStamp = 0;
    uint64_t expectedPhysicsRevision = 0;
    uint32_t outputId = 0;
    uint32_t outputGeneration = 0;
    bool shadowQueriesEnabled = true;
};

struct SpatialDebugOverlayResult {
    bool snapshotAccepted = false;
    bool traversalStatusAvailable = false;
    std::size_t drawCommands = 0;
    std::string status;
};

/// @brief Append validated snapshot values without retaining producer or scene owners.
/// @note Per-target commands are capped by settings and the output is globally capped at 8192.
SpatialDebugOverlayResult BuildRenderBvhDebugCommands(const renderer::RenderBvhDebugSnapshot& snapshot,
    const SpatialDebugTargetSettings& settings, const SpatialDebugOverlayContext& context,
    std::vector<scene::ScriptDebugDrawCommand>& commands);
SpatialDebugOverlayResult BuildPhysicsSpatialDebugCommands(const scene::PhysicsSpatialDebugSnapshot& snapshot,
    const SpatialDebugTargetSettings& settings, const SpatialDebugOverlayContext& context,
    std::vector<scene::ScriptDebugDrawCommand>& commands);

void UpdateSpatialDebugRequests(EditorContext& ctx, scene::Scene* scene, bool runtimeActive,
    uint32_t outputId, uint32_t outputGeneration);
void BuildSpatialDebugOverlay(EditorContext& ctx, const scene::Scene& scene,
    uint32_t outputId, uint32_t outputGeneration, uint64_t currentResourceFrameStamp,
    std::vector<scene::ScriptDebugDrawCommand>& commands, bool shadowQueriesEnabled = true);
/// @brief Immediately retire both publications on Stop and Shutdown.
void ResetSpatialDebugCapture();

} /// @note namespace fbzz::editor
