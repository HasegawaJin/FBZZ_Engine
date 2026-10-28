/// @file    RenderTerrainInput.hpp
/// @brief   地形パッチと解決済み層資源の入力。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Pipeline/RenderConstants.hpp>
namespace fbzz::renderer {
/// @note 32-byte layout shared with TerrainGeometry.hlsli; heights have a one-sample border.
struct TerrainGpuPatch {
    uint32_t originX = 0, originZ = 0;
    uint32_t columns = 0, rows = 0;
    uint32_t terrainColumns = 0, terrainRows = 0;
    float cellSize = 1.0f;
    uint32_t padding = 0;
};
static_assert(sizeof(TerrainGpuPatch) == 32);

struct RenderTerrainPatch {
    renderer::ResourceHandle<renderer::BufferTag> vertexBuffer;
    std::array<renderer::ResourceHandle<renderer::BufferTag>, 3> indexBufferLOD;
    std::array<uint32_t, 3> indexCountLOD = {};
    math::Vector3 aabbMin;
    math::Vector3 aabbMax;
    TerrainGpuPatch gpuPatch{};
    std::array<uint32_t, 3> gpuStartIndexLOD{};
};

struct RenderTerrainInput {
    uint32_t layer = 0;
    bool selected = false;
    bool fiberSurface = false;
    bool fiberDistanceLod = true;
    float fiberLodFar = 40.0f;
    float chunkWorldSize = 1;
    TerrainObjectCB constants{};
    ResourceHandle<TextureTag> splatIndices, splatWeights;
    std::vector<RenderTerrainPatch> patches;
    /// @note Immutable geometry resources are shared by camera, shadow and selection passes.
    ResourceHandle<StructuredBufferTag> gpuHeights, gpuPatches;
    ResourceHandle<BufferTag> gpuIndices;
    uint32_t gpuIndexCount = 0, gpuVertexCapacity = 0;
};
} /// @note namespace fbzz::renderer
