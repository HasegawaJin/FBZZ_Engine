/// @file    RenderTerrainInput.hpp
/// @brief   地形パッチと解決済み層資源の入力。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Pipeline/RenderConstants.hpp>
#include <memory>
namespace fbzz::renderer {
inline constexpr size_t TERRAIN_FAR_LOD_INDEX = 3;
inline constexpr std::array<int, 4> TERRAIN_LOD_STEPS = {1, 2, 4, 8};

struct TerrainOcclusionColumn {
    math::Vector4 rectangle;
    float minimumHeight = 0;
};
/// @note 描画と同じ穴・境界接続を持つ不変の CPU 形状。地形の編集時だけ再生成する。
struct TerrainOcclusionGeometry {
    std::vector<float> heights;
    std::array<std::vector<uint16_t>, TERRAIN_LOD_STEPS.size()> indices;
    /// @note 隣接列と重なる footprint 全域の下限。全 LOD の境界に揃え、形状更新時だけ生成する。
    std::vector<TerrainOcclusionColumn> columns;
};
/// @note 32-byte layout shared with TerrainGeometry.hlsli; heights have a one-sample border.
struct TerrainGpuPatch {
    uint32_t originX = 0, originZ = 0;
    uint32_t columns = 0, rows = 0;
    uint32_t terrainColumns = 0, terrainRows = 0;
    float cellSize = 1.0f;
    uint32_t padding = 0;
};
static_assert(sizeof(TerrainGpuPatch) == 32);

struct TerrainStripIndices {
    ResourceHandle<BufferTag> buffer;
    uint32_t count = 0;
};

struct RenderTerrainPatch {
    std::shared_ptr<const TerrainOcclusionGeometry> occlusion;
    renderer::ResourceHandle<renderer::BufferTag> vertexBuffer;
    std::array<renderer::ResourceHandle<renderer::BufferTag>, TERRAIN_LOD_STEPS.size()> indexBufferLOD;
    std::array<uint32_t, TERRAIN_LOD_STEPS.size()> indexCountLOD = {};
    math::Vector3 aabbMin;
    math::Vector3 aabbMax;
    TerrainGpuPatch gpuPatch{};
    std::array<uint32_t, TERRAIN_LOD_STEPS.size()> gpuStartIndexLOD{};
    TerrainStripIndices lod0Strip;
};

struct RenderTerrainInput {
    uint32_t layer = 0;
    bool selected = false;
    bool fiberSurface = false;
    bool fiberDistanceLod = true;
    float fiberLodFar = 40.0f;
    float chunkWorldSize = 1;
    float farLodDistance = 0.0f;
    size_t LODCount() const { return farLodDistance > 0.0f ? TERRAIN_LOD_STEPS.size() : TERRAIN_FAR_LOD_INDEX; }
    TerrainObjectCB constants{};
    ResourceHandle<TextureTag> splatIndices, splatWeights;
    std::vector<RenderTerrainPatch> patches;
    /// @note Immutable geometry resources are shared by camera, shadow and selection passes.
    ResourceHandle<StructuredBufferTag> gpuHeights, gpuPatches;
    ResourceHandle<BufferTag> gpuIndices;
    uint32_t gpuIndexCount = 0, gpuVertexCapacity = 0;
    /// @note 高さ、穴、境界と変換の版をホストが検証できる地形に限る。
    bool shadowBakeStatic = false;
    uint64_t shadowBakeGeometryHash = 0;
};
} /// @note namespace fbzz::renderer
