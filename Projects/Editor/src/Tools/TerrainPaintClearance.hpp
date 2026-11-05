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
    std::unordered_map<uint64_t, std::vector<size_t>> cells;
    std::vector<size_t> global;
};
struct TerrainPaintClearance {
    std::vector<TerrainPaintEdge> water, paths;
    TerrainPaintEdgeIndex waterIndex, pathIndex;
    [[nodiscard]] bool NearWater(const math::Vector3& position, float distance) const;
    [[nodiscard]] bool NearPath(const math::Vector3& position, float distance) const;
};
[[nodiscard]] TerrainPaintClearance BuildTerrainPaintClearance(const scene::TerrainComponent& terrain,
    const scene::Transform& transform, const TerrainScatterBrush& region, std::span<const int> excludedLayers,
    std::span<const std::pair<const scene::WaterComponent*, const scene::Transform*>> water, float waterMargin, float pathMargin);
} /// @note namespace fbzz::editor
