/// @file    TerrainSurfaceGeometry.hpp
/// @brief   地形が所有する洞窟・崖・張り出しの三角形を検証する共通処理。
/// @author  Hasegawa Jin
/// @date    2026-10-11
#pragma once
#include <Math/LinearAlgebra/Vector3.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace fbzz::scene {
/// @note 位置は地形ローカル座標 [m]、添字は三角形リスト。頂点順で法線を空間側に向ける。
/// @note 両配列が空なら従来の高さ場を表す。不完全または縮退した形状は拒否する。
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
}
