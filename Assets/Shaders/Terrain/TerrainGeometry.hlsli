/// @file    TerrainGeometry.hlsli
/// @brief   Height samples generate terrain position, normal, tangent and UV on the GPU.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#ifndef FBZZ_TERRAIN_GEOMETRY_HLSLI
#define FBZZ_TERRAIN_GEOMETRY_HLSLI
#include "Common/Binding.hlsli"
#include "Common/BindlessIndices.hlsli"

/// @note Layout matches Graphics/Effects/RenderTerrainInput.hpp; samples contain a one-vertex border.
struct TerrainGpuPatch
{
    uint originX, originZ;
    uint columns, rows;
    uint terrainColumns, terrainRows;
    float cellSize;
    uint padding;
};
FBZZ_VS_SBUFFER(float, g_terrainHeights, VS_SB_BUFFER0_SLOT);
FBZZ_VS_SBUFFER(TerrainGpuPatch, g_terrainPatches, VS_SB_BUFFER1_SLOT);
/// @note Indexed indirect commands supply the patch ordinal; direct draws reset the root constant to zero.
/// @see https://learn.microsoft.com/en-us/windows/win32/direct3d12/indirect-drawing - root constant argument records.
cbuffer TerrainGeometryDrawConstants : register(b15)
{
    uint g_terrainPatchIndex;
};
struct TerrainGeometryVertex
{
    float3 position;
    float3 normal;
    float3 tangent;
    float2 uv;
};
/// @note SV_VertexID addresses the selected LOD's local grid indices, with BaseVertexLocation zero.
/// @see https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-semantics - SV_VertexID.
TerrainGeometryVertex EvaluateTerrainGeometry(uint vertexId)
{
    const TerrainGpuPatch patch = g_terrainPatches[g_terrainPatchIndex];
    const uint2 sample = uint2(patch.originX + vertexId % patch.columns,
                              patch.originZ + vertexId / patch.columns);
    const uint stride = patch.terrainColumns + 2;
    const uint index = (sample.y + 1) * stride + sample.x + 1;
    const float height = g_terrainHeights[index];
    const float dhdx = (g_terrainHeights[index + 1] - g_terrainHeights[index - 1]) / (2.0f * patch.cellSize);
    const float dhdz = (g_terrainHeights[index + stride] - g_terrainHeights[index - stride]) / (2.0f * patch.cellSize);
    TerrainGeometryVertex result;
    result.position = float3(sample.x * patch.cellSize, height, sample.y * patch.cellSize);
    result.normal = normalize(float3(-dhdx, 1.0f, -dhdz));
    result.tangent = normalize(float3(patch.cellSize, dhdx * patch.cellSize, 0.0f));
    result.uv = float2(sample) / float2(patch.terrainColumns - 1, patch.terrainRows - 1);
    return result;
}
#endif
