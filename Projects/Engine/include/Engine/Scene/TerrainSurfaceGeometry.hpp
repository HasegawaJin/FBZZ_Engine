/// @file    TerrainSurfaceGeometry.hpp
/// @brief   Shared validation for terrain-owned cave, cliff and overhang triangles.
/// @author  Hasegawa Jin
/// @date    2026-10-11
#pragma once
#include <Math/Vector3.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace fbzz::scene {
/// @note Positions are terrain-local metres, indices are triangle lists, winding faces empty space.
/// @note Both arrays empty represents a legacy heightfield; incomplete or degenerate geometry is rejected.
inline bool IsValidTerrainSurfaceGeometry(const std::vector<math::Vector3>& positions,
    const std::vector<uint32_t>& indices)
{
    if (positions.empty() || indices.empty()) return positions.empty() && indices.empty();
    if (positions.size() > std::numeric_limits<uint32_t>::max() || indices.size() > std::numeric_limits<uint32_t>::max()
        || indices.size() % 3 != 0) return false;
    for (const auto& v : positions)
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) return false;
    for (size_t i = 0; i < indices.size(); i += 3) {
        const auto a = indices[i], b = indices[i + 1], c = indices[i + 2];
        if (a >= positions.size() || b >= positions.size() || c >= positions.size()) return false;
        const auto normal = math::Vector3::Cross(positions[b] - positions[a], positions[c] - positions[a]);
        if (!std::isfinite(normal.LengthSq()) || normal.LengthSq() < 1e-10f) return false;
    }
    return true;
}
} /// @note namespace fbzz::scene
