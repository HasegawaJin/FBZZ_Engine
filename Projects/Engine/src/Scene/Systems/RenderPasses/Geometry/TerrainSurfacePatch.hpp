/// @file    TerrainSurfacePatch.hpp
/// @brief   CPU terrain surface vertices and full three-dimensional culling bounds.
/// @author  Hasegawa Jin
/// @date    2026-10-11
#pragma once
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Math/Vector2.hpp>
#include <algorithm>
#include <vector>
namespace fbzz::scene {
/// @note GPU layout matches TerrainVSInput: POSITION/NORMAL/TANGENT float3, TEXCOORD float2.
struct TerrainVertex {
    math::Vector3 position;
    math::Vector3 normal;
    math::Vector3 tangent;
    math::Vector2 uv;
};
static_assert(sizeof(TerrainVertex) == 44);
constexpr size_t TERRAIN_SURFACE_PATCH_INDICES = 768;
struct TerrainSurfacePatchData {
    std::vector<TerrainVertex> vertices;
    std::vector<uint32_t> indices;
    math::Vector3 aabbMin, aabbMax;
};
/// @pre Valid nonempty surface geometry, first is a triangle boundary within surfaceIndices.
/// @note All LODs use this topology; caves must not lose roofs or close entrances when LOD changes.
inline TerrainSurfacePatchData BuildTerrainSurfacePatch(const TerrainComponent& terrain, size_t first)
{
    TerrainSurfacePatchData result;
    const size_t end = (std::min)(first + TERRAIN_SURFACE_PATCH_INDICES, terrain.surfaceIndices.size());
    result.vertices.reserve(end - first); result.indices.reserve(end - first);
    result.aabbMin = {1e30f,1e30f,1e30f}; result.aabbMax = {-1e30f,-1e30f,-1e30f};
    for (size_t i = first; i < end; i += 3) {
        const auto a = terrain.surfacePositions[terrain.surfaceIndices[i]];
        const auto b = terrain.surfacePositions[terrain.surfaceIndices[i + 1]];
        const auto c = terrain.surfacePositions[terrain.surfaceIndices[i + 2]];
        const auto normal = math::Vector3::Cross(b - a, c - a).Normalized();
        const auto tangent = (b - a).Normalized();
        for (const auto& v : {a,b,c}) {
            result.indices.push_back(static_cast<uint32_t>(result.vertices.size()));
            result.vertices.push_back({v,normal,tangent,{v.x / ((terrain.columns - 1) * terrain.cellSize),
                v.z / ((terrain.rows - 1) * terrain.cellSize)}});
            result.aabbMin = {(std::min)(result.aabbMin.x,v.x),(std::min)(result.aabbMin.y,v.y),(std::min)(result.aabbMin.z,v.z)};
            result.aabbMax = {(std::max)(result.aabbMax.x,v.x),(std::max)(result.aabbMax.y,v.y),(std::max)(result.aabbMax.z,v.z)};
        }
    }
    return result;
}
} /// @note namespace fbzz::scene
