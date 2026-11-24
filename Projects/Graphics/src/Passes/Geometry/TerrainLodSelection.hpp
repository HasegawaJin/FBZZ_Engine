/// @file    TerrainLodSelection.hpp
/// @brief   描画と影で共通の地形 LOD と植生保護の距離判定。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

#include <Graphics/Effects/RenderTerrainInput.hpp>
#include <algorithm>
#include <array>
#include <cmath>

namespace fbzz::renderer::terrain_lod_detail {

inline std::array<math::Vector3, 2> TerrainWorldBounds(const math::Matrix4& world,
    const math::Vector3& localMin, const math::Vector3& localMax)
{
    const auto localCenter = (localMin + localMax) * 0.5f;
    const auto localExtents = (localMax - localMin) * 0.5f;
    const auto center = world * math::Vector4{localCenter, 1.0f};
    const auto extent = [&](uint32_t row) {
        return std::abs(world.m[row][0]) * localExtents.x + std::abs(world.m[row][1]) * localExtents.y
            + std::abs(world.m[row][2]) * localExtents.z;
    };
    const math::Vector3 worldCenter{center.x, center.y, center.z};
    const math::Vector3 worldExtents{extent(0), extent(1), extent(2)};
    return {worldCenter - worldExtents, worldCenter + worldExtents};
}
inline bool PreserveFiberTerrainDetail(const RenderTerrainInput& input, const RenderTerrainPatch& patch,
    const math::Vector3& cameraPosition)
{
    if (!input.fiberSurface) return false;
    if (!input.fiberDistanceLod) return true;
    const auto bounds = TerrainWorldBounds(input.constants.worldMatrix,patch.aabbMin,patch.aabbMax);
    const math::Vector3 nearest{std::clamp(cameraPosition.x,bounds[0].x,bounds[1].x),
        std::clamp(cameraPosition.y,bounds[0].y,bounds[1].y),std::clamp(cameraPosition.z,bounds[0].z,bounds[1].z)};
    return (nearest-cameraPosition).LengthSq() <= input.fiberLodFar*input.fiberLodFar;
}

inline float TerrainLODDistanceSquared(const RenderTerrainInput& input, const RenderTerrainPatch& patch,
    const math::Vector3& cameraPosition)
{
    const auto localCenter = (patch.aabbMin + patch.aabbMax) * 0.5f;
    const auto worldCenter = input.constants.worldMatrix * math::Vector4{localCenter, 1.0f};
    return (math::Vector3{worldCenter.x, worldCenter.y, worldCenter.z} - cameraPosition).LengthSq();
}

inline bool UseFarTerrainLOD(const RenderTerrainInput& input, const RenderTerrainPatch& patch, float distanceSquared)
{
    if (input.farLodDistance <= 0.0f || !patch.indexBufferLOD[TERRAIN_FAR_LOD_INDEX].IsValid()) return false;
    /// @note 穴を含む粗い格子は細分化して保持するため、三角形が減らない場合もある。
    if (patch.indexCountLOD[TERRAIN_FAR_LOD_INDEX] >= patch.indexCountLOD[TERRAIN_FAR_LOD_INDEX - 1]) return false;
    const float farDistance = std::max(input.farLodDistance, input.chunkWorldSize * 6.0f);
    return distanceSquared >= farDistance * farDistance;
}

/// @brief 地形の描画と影で同じチャンク LOD を選ぶ。
/// @note 異なるメッシュで自己影を描くと粗い三角形が表示面を貫き、斜面に偽の影が出る。
inline int SelectTerrainLOD(const RenderTerrainInput& input, const RenderTerrainPatch& chunk,
                            const math::Vector3& cameraPosition)
{
    if (PreserveFiberTerrainDetail(input,chunk,cameraPosition)) return 0;
    const float distanceSquared = TerrainLODDistanceSquared(input, chunk, cameraPosition);
    if (UseFarTerrainLOD(input, chunk, distanceSquared)) return static_cast<int>(TERRAIN_FAR_LOD_INDEX);
    const float nearDistance = input.chunkWorldSize * 2.0f;
    const float midDistance = input.chunkWorldSize * 6.0f;
    return distanceSquared < nearDistance * nearDistance ? 0
         : distanceSquared < midDistance * midDistance ? 1 : 2;
}

} /// @note namespace fbzz::renderer::terrain_lod_detail
