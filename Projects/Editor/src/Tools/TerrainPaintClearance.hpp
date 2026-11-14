/// @file    TerrainPaintClearance.hpp
/// @brief   水際と地形マスクの境界を使う配置距離判定。
/// @author  Hasegawa Jin
/// @date    2026-11-05
#pragma once
#include "TerrainScatter.hpp"
#include <unordered_map>
namespace fbzz::scene { struct WaterComponent; }
namespace fbzz::editor {
struct TerrainPaintEdge { math::Vector3 a, b; };
struct TerrainPaintEdgeIndex {
    float cellSize = 4;
    int minX = 0, minZ = 0, maxX = -1, maxZ = -1;
    std::unordered_map<uint64_t, std::vector<size_t>> cells;
    std::vector<size_t> global;
};
struct TerrainPaintClearance {
    std::vector<TerrainPaintEdge> water, paths;
    TerrainPaintEdgeIndex waterIndex, pathIndex;
    bool budgetExceeded = false;
    [[nodiscard]] bool NearWater(const math::Vector3& position, float distance) const;
    /// @return The original 16-step shoreline distance with vertical clearance, or 1,000,000 m on a miss.
    [[nodiscard]] float WaterDistance(const math::Vector3& position, float radius) const;
    [[nodiscard]] bool NearPath(const math::Vector3& position, float distance) const;
    /// @return The nearest shoreline point within the world XZ radius, without a vertical filter.
    [[nodiscard]] bool NearestWater(const math::Vector3& position, float radius, math::Vector3& nearest) const;
    [[nodiscard]] float PathDistance(const math::Vector3& position, float radius) const;
};
/// @param remainingWork Optional shared budget for contour cells and water outline segments.
/// @param includeFiniteWaterOutlines Disable when the same world-space outlines were indexed for another tile.
/// @return A result with budgetExceeded is incomplete and must not be used for placement.
[[nodiscard]] TerrainPaintClearance BuildTerrainPaintClearance(const scene::TerrainComponent& terrain,
    const scene::Transform& transform, const TerrainScatterBrush& region, std::span<const int> excludedLayers,
    std::span<const std::pair<const scene::WaterComponent*, const scene::Transform*>> water, float waterMargin,
    float pathMargin, bool combinePathLayers = false, std::size_t* remainingWork = nullptr,
    bool includeFiniteWaterOutlines = true);
} /// @note namespace fbzz::editor
