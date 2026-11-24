/// @file    RenderTerrainExtraction.hpp
/// @brief   地形の抽出で共有するジオメトリ・マテリアル準備。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

#include "TerrainRenderCache.hpp"
#include "RenderPasses/Geometry/TerrainSurfacePatch.hpp"
#include <array>
#include <unordered_map>
#include <vector>

namespace fbzz::scene { struct RenderPassContext; struct TerrainGridComponent; }

namespace fbzz::scene::terrain_extraction_detail {

struct TerrainNeighbors {
    const TerrainComponent* north = nullptr;
    const TerrainComponent* south = nullptr;
    const TerrainComponent* west  = nullptr;
    const TerrainComponent* east  = nullptr;
    const TerrainComponent* northWest = nullptr;
    const TerrainComponent* northEast = nullptr;
    const TerrainComponent* southWest = nullptr;
    const TerrainComponent* southEast = nullptr;
};

class TerrainGridLookup {
public:
    explicit TerrainGridLookup(const TerrainGridComponent* terrainGrid);
    [[nodiscard]] const std::array<int, 2>* Find(EntityID entity) const;
    [[nodiscard]] const TerrainGridComponent* Grid() const { return m_grid; }

private:
    const TerrainGridComponent* m_grid;
    std::unordered_map<uint64_t, std::array<int, 2>> m_positions;
};

struct TerrainChunkUpload {
    TerrainChunk* chunk;
    size_t firstVertex;
    size_t firstIndex;
};

struct TerrainChunkScratch {
    std::vector<float> heights;
    std::vector<TerrainVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<TerrainChunkUpload> uploads;
    std::vector<const TerrainChunk*> drawChunks;
    int minX = 0, minZ = 0;
    size_t stride = 0;

    [[nodiscard]] const float* HeightRow(int z) const
    {
        return heights.data() + static_cast<size_t>(z - minZ) * stride;
    }
};

struct TerrainLayerBinding {
    uint32_t layerCount;
    uint32_t bufferIndex;
    bool usesAutoBlend;
};

[[nodiscard]] const TerrainGridComponent* FindActiveTerrainGrid(Scene& scene);
[[nodiscard]] TerrainNeighbors ResolveTerrainNeighbors(Scene& scene, const TerrainGridLookup& gridLookup, EntityID entity);
void EraseTerrainChunkCache(TerrainGpuState& gpu, renderer::ResourceManager& resources);
void ReleaseRemovedTerrainResources(TerrainRenderCache& cache, Scene& scene, renderer::ResourceManager& resources);
void RefreshTerrainGeometry(TerrainRenderCache& cache, Scene& scene, const TerrainGridLookup& gridLookup,
    EntityID entity, renderer::ResourceManager& resources);
void FillTerrainHeightRow(const TerrainComponent& terrain, const TerrainNeighbors& neighbors,
    int firstX, int lastX, int rowZ, float* output, bool canonicalZero = false);
void PrepareTerrainPatches(TerrainGpuState& gpu, const TerrainComponent& terrain, const TerrainNeighbors& neighbors,
    renderer::RenderTerrainInput& input, TerrainChunkScratch& scratch, const RenderPassContext& context);
TerrainLayerBinding PrepareTerrainLayers(TerrainRenderCache& cache, TerrainGpuState& gpu, TerrainComponent& terrain,
    renderer::ResourceManager& resources);
bool ApplyTerrainSurfaceMaterial(TerrainRenderCache& cache, const TerrainComponent& terrain, TerrainGpuState& gpu,
    renderer::RenderTerrainInput& surface, renderer::ResourceManager& resources);
void ExtractTerrainSurface(TerrainRenderCache& cache, const TerrainComponent& terrain, TerrainGpuState& gpu,
    const renderer::RenderTerrainInput& base, renderer::ResourceManager& resources, renderer::RenderScene& output);
void UpdateSplatTextures(TerrainRenderCache& cache, TerrainGpuState& gpu, TerrainComponent& terrain,
    renderer::IRenderer& renderer, renderer::ResourceManager& resources);
void PrepareTerrainGpuGeometry(TerrainGpuState& state, const TerrainComponent& terrain,
    const TerrainNeighbors& neighbors, renderer::RenderTerrainInput& input,
    TerrainRenderCache& cache, renderer::IRenderer& renderer, renderer::ResourceManager& resources);
[[nodiscard]] uint64_t TerrainShadowBakeGeometryHash(const TerrainComponent& terrain,
    const std::vector<renderer::RenderTerrainPatch>& patches);

} /// @note namespace fbzz::scene::terrain_extraction_detail
