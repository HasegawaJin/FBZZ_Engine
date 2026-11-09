/// @file    TerrainRenderCache.cpp
/// @brief   地形資源を Scene と GPU 作成元の寿命で返却する。
/// @author  Hasegawa Jin
/// @date    2026-10-30
#include "TerrainRenderCache.hpp"
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/RenderTerrainExtractor.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <algorithm>
#include <memory>

namespace fbzz::scene {
namespace {
using CacheList = std::vector<std::unique_ptr<TerrainRenderCache>>;
std::unordered_map<uint64_t, CacheList> s_caches;
renderer::ResourceManager* ResolveOwner(const TerrainRenderCache& cache)
{
    auto* owner = renderer::ResourceManager::FindOwner(cache.owner);
    return owner && owner->GetResetVersion() == cache.resetVersion ? owner : nullptr;
}
} /// @note namespace

void ReleaseTerrainChunkBuffers(renderer::ResourceManager& resources, TerrainChunk& chunk)
{
    resources.Release(chunk.vertexBuffer);
    for (auto& buffer : chunk.indexBufferLOD) resources.Release(buffer);
}
void ReleaseTerrainGpuState(TerrainGpuState& state, renderer::ResourceManager& resources)
{
    resources.Release(state.splatIndices);
    resources.Release(state.splatWeights);
    resources.Release(state.layerBuffer);
    resources.Release(state.surfaceLayerBuffer);
    resources.Release(state.surfaceSplatIndices);
    resources.Release(state.surfaceSplatWeights);
    resources.Release(state.heights);
    resources.Release(state.patches);
    resources.Release(state.indices);
    for (auto& strip : state.rasterStrips) resources.Release(strip.indices.buffer);
    state = {};
}


TerrainRenderCache::~TerrainRenderCache()
{
    if (auto* resources = ResolveOwner(*this)) {
        for (auto& [key, chunk] : chunks) ReleaseTerrainChunkBuffers(*resources, chunk);
        for (auto& [index, state] : entities) ReleaseTerrainGpuState(state, *resources);
        ReleaseTerrainFallbackTextures(*resources, fallback);
    }
}

void TerrainRenderCache::ReleaseEntity(EntityID entity)
{
    auto* resources = ResolveOwner(*this);
    std::erase_if(chunks, [&](auto& entry) {
        if (entry.first.entityId != entity) return false;
        if (resources) ReleaseTerrainChunkBuffers(*resources, entry.second);
        return true;
    });
    const auto found = entities.find(entity.index);
    if (found != entities.end() && found->second.entityId == entity) {
        if (resources) ReleaseTerrainGpuState(found->second, *resources);
        entities.erase(found);
    }
}

TerrainRenderCache& AcquireTerrainRenderCache(Scene& scene, renderer::ResourceManager& resources)
{
    auto& caches = s_caches[scene.GetRenderSceneGeneration()];
    std::erase_if(caches, [](const auto& cache) { return !ResolveOwner(*cache); });
    for (auto& cache : caches)
        if (cache->owner == resources.GetInstanceId()) return *cache;
    auto cache = std::make_unique<TerrainRenderCache>();
    cache->owner = resources.GetInstanceId();
    cache->resetVersion = resources.GetResetVersion();
    cache->fallback = CreateTerrainFallbackTextures(resources);
    caches.push_back(std::move(cache));
    return *caches.back();
}

void ReleaseTerrainRenderResources(Scene& scene)
{
    s_caches.erase(scene.GetRenderSceneGeneration());
}

void ReleaseTerrainRenderResources(Scene& scene, EntityID entity)
{
    const auto found = s_caches.find(scene.GetRenderSceneGeneration());
    if (found == s_caches.end()) return;
    for (auto& cache : found->second) cache->ReleaseEntity(entity);
}
} /// @note namespace fbzz::scene
