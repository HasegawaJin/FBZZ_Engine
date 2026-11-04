/// @file    RenderBvhDebug.hpp
/// @brief   Bounded value diagnostics for the production shadow-candidate BVH.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#pragma once
#include <Graphics/Api.hpp>

#include <Math/Vector3.hpp>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace fbzz::renderer {

inline constexpr uint32_t RENDER_BVH_DEBUG_MAX_NODES = 4096;
inline constexpr uint32_t RENDER_BVH_DEBUG_MAX_SELECTION_SOURCES = 4096;
inline constexpr uint32_t RENDER_BVH_DEBUG_NO_NODE = std::numeric_limits<uint32_t>::max();

enum class RenderBvhDebugAvailability : uint8_t {
    WAITING, READY, LINEAR_MODE, NO_CURRENT_TREE, NO_SHADOW_VOLUMES, NUMERIC_FALLBACK, VALIDATION_FAILED
};
enum class RenderBvhDebugPartition : uint8_t { STATIC, DYNAMIC };
enum class RenderBvhDebugState : uint8_t { UNOBSERVED, VISITED, REJECTED, CANDIDATE };

struct RenderBvhDebugRequest {
    bool enabled = false;
    uint64_t sceneGeneration = 0;
    /// @note A nonzero output ID restricts capture to that exact render-target generation; zero accepts any view.
    uint32_t viewOutputId = 0, viewOutputGeneration = 0;
    /// @note maxNodes limits hierarchy nodes examined as well as copied; depth filtering may exhaust work before filling the output.
    uint32_t minDepth = 0, maxDepth = 32, maxNodes = 512;
    bool selectedOnly = false;
    /// @note UINT32_MAX means no selected entity; generation zero is a valid entity generation.
    uint32_t sourceIndex = std::numeric_limits<uint32_t>::max(), sourceGeneration = 0;
    bool operator==(const RenderBvhDebugRequest&) const = default;
};

/// @note Bounds are world-space values from an actual tree; parentIndex addresses that partition's original tree, even when filtered out.
/// @note VISITED means a surviving internal node; REJECTED means an observed bounds/sphere rejection; CANDIDATE means the observed leaf passed the union query.
/// @note UNOBSERVED includes descendants skipped after an ancestor rejection; no state is inferred for those descendants.
struct RenderBvhDebugNode {
    math::Vector3 low{}, high{};
    uint32_t nodeIndex = 0, parentIndex = RENDER_BVH_DEBUG_NO_NODE, depth = 0;
    uint32_t sourceIndex = 0, sourceGeneration = 0;
    RenderBvhDebugPartition partition = RenderBvhDebugPartition::STATIC;
    RenderBvhDebugState state = RenderBvhDebugState::UNOBSERVED;
    bool leaf = false, skinned = false, selectedPath = false;
};

/// @note Values only: no Scene, component, RenderScene, resource or tree owner is retained across the render/UI boundary.
/// @note Observations belong to one production shadow-light-volume union, never camera visibility or individual light queries.
struct RenderBvhDebugSnapshot {
    RenderBvhDebugAvailability availability = RenderBvhDebugAvailability::WAITING;
    uint64_t requestRevision = 0, sceneGeneration = 0, snapshotSerial = 0;
    uint64_t frameStamp = 0, contextFrameStamp = 0;
    uint32_t viewOutputId = 0, viewOutputGeneration = 0;
    uint32_t shadowVolumeCount = 0;
    uint64_t totalNodes = 0, unboundedSources = 0, visitedNodes = 0, testedLeaves = 0, candidates = 0;
    uint32_t examinedNodes = 0, selectionSourcesExamined = 0;
    /// @note Selected lookup examines at most MAX_SELECTION_SOURCES ordinals; selectedFound=false is inconclusive while selectedLookupTruncated=true.
    bool truncated = false, selectedFound = false, selectedLookupTruncated = false;
    std::vector<RenderBvhDebugNode> nodes;
};

/// @note All methods are thread-safe; GetSnapshot shares an immutable bounded publication, and a changed request retires the previous publication.
/// @note The host must disable/invalidate on scene deletion/switch and stop; an in-flight producer cannot republish an invalidated revision.
/// @note Unset/linear production mode remains unchanged; availability explains that FBZZ_MESH_SPATIAL_MODE=bvh/validate/auto is required at process startup.
class RenderBvhDebug {
public:
    FBZZ_GRAPHICS_API static void SetRequest(const RenderBvhDebugRequest& request);
    [[nodiscard]] FBZZ_GRAPHICS_API static std::shared_ptr<const RenderBvhDebugSnapshot> GetSnapshot();
    /// @note Disables capture and retires the publication; SetRequest is required to start a new capture lifetime.
    FBZZ_GRAPHICS_API static void Invalidate();
};

} /// @note namespace fbzz::renderer
