/// @file    TerrainRenderCache.hpp
/// @brief   Scene 世代と作成元に属する地形 GPU キャッシュ。
/// @author  Hasegawa Jin
/// @date    2026-10-30
#pragma once
#include <Engine/Scene/Entity.hpp>
#include <Engine/Asset/AssetHandle.hpp>
#include <Engine/Scene/Systems/RenderPasses/Geometry/TerrainRenderPass.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::asset { struct MaterialAsset; }
namespace fbzz::scene {
class Scene;
using TerrainChunk = renderer::RenderTerrainPatch;
struct TerrainChunkKey {
    EntityID entityId;
    uint32_t chunkX = 0;
    uint32_t chunkZ = 0;
    bool operator==(const TerrainChunkKey& o) const = default;
};


struct TerrainChunkKeyHash {
    size_t operator()(const TerrainChunkKey& key) const noexcept
    {
        size_t hash = std::hash<uint32_t>{}(key.entityId.index);
        for (uint32_t value : {key.entityId.generation, key.chunkX, key.chunkZ})
            hash ^= std::hash<uint32_t>{}(value) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
        return hash;
    }
};
struct TerrainGpuState {
    struct RasterStrip {
        renderer::TerrainStripIndices indices;
        uint32_t columns = 0, rows = 0;
    };

    EntityID entityId = EntityID::INVALID;
    uint64_t surfaceRevision = UINT64_MAX, vegetationRevision = UINT64_MAX;
    renderer::ResourceHandle<renderer::TextureTag> splatIndices;   ///< @note 所有
    renderer::ResourceHandle<renderer::TextureTag> splatWeights;   ///< @note 所有
    renderer::ResourceHandle<renderer::StructuredBufferTag> layerBuffer; ///< @note 所有
    uint32_t layerCapacity = 0;
    /// @note 直前に転送したバイト列。同じなら Update を省く。
    std::vector<TerrainLayerGpu> uploadedLayers;
    renderer::ResourceHandle<renderer::StructuredBufferTag> heights, patches;
    renderer::ResourceHandle<renderer::BufferTag> indices;
    std::vector<decltype(renderer::RenderTerrainPatch::gpuStartIndexLOD)> indexStarts;
    uint32_t indexCount = 0, vertexCapacity = 0;
    size_t lodCount = 0;
    /// @note Full, right-edge, bottom-edge, and corner grids share their draw-only buffers by dimensions.
    std::array<RasterStrip, 4> rasterStrips;
    bool geometryDirty = true;
    bool heightsDirty = true;
    uint64_t shadowBakeGeometryHash = 0;
    renderer::ResourceHandle<renderer::StructuredBufferTag> surfaceLayerBuffer;
    renderer::ResourceHandle<renderer::TextureTag> surfaceSplatIndices, surfaceSplatWeights;
    std::optional<TerrainLayerGpu> uploadedSurfaceLayer;
};


/// @note 資源管理スレッド専用。Scene 終了時に全作成元へ返し、終了済み・Reset 済みの作成元は解決しない。
struct TerrainRenderCache {
    TerrainRenderCache() = default;
    TerrainRenderCache(const TerrainRenderCache&) = delete;
    TerrainRenderCache& operator=(const TerrainRenderCache&) = delete;
    uint64_t owner = 0, resetVersion = 0;
    TerrainFallbackTextures fallback;
    std::unordered_map<TerrainChunkKey, TerrainChunk, TerrainChunkKeyHash> chunks;
    std::unordered_map<uint32_t, TerrainGpuState> entities;
    uint64_t materialIndexRevision = UINT64_MAX;
    int materialAssetGeneration = -1;
    std::unordered_map<std::string, asset::AssetHandle<asset::MaterialAsset>> materials;
    ~TerrainRenderCache();
    void ReleaseEntity(EntityID entity);
};
TerrainRenderCache& AcquireTerrainRenderCache(Scene& scene, renderer::ResourceManager& resources);
void ReleaseTerrainChunkBuffers(renderer::ResourceManager& resources, TerrainChunk& chunk);
void ReleaseTerrainGpuState(TerrainGpuState& state, renderer::ResourceManager& resources);
} /// @note namespace fbzz::scene
