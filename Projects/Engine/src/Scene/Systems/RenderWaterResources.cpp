/// @file    RenderWaterResources.cpp
/// @brief   シーンと GPU リソース管理元に対応する水面キャッシュの寿命と更新。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "RenderWaterResources.hpp"
#include <Core/Profiler/ProfileScope.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/RenderWaterExtractor.hpp>
#include <Engine/Scene/WaterSurfaceQuery.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <algorithm>

namespace fbzz::scene::water_render_detail {

namespace {

using WaterCacheList = std::vector<std::unique_ptr<WaterRenderCache>>;
std::unordered_map<uint64_t, WaterCacheList> s_sceneWaterCaches;

}

WaterPatchGrid::WaterPatchGrid(const renderer::ResourceManager& resources)
    : resourceOwner(resources.GetInstanceId()), resetVersion(resources.GetResetVersion())
{
}

WaterPatchGrid::~WaterPatchGrid()
{
    auto* resources = renderer::ResourceManager::FindOwner(resourceOwner);
    if (!resources || resources->GetResetVersion() != resetVersion) return;
    if (vertexBuffer.IsValid()) resources->Release(vertexBuffer);
    for (const auto buffer : indexBuffers) if (buffer.IsValid()) resources->Release(buffer);
    for (const auto& strip : strips) if (strip.buffer.IsValid()) resources->Release(strip.buffer);
}

WaterRenderCache::WaterRenderCache(const renderer::ResourceManager& resources)
    : m_resourceOwner(resources.GetInstanceId()), m_resetVersion(resources.GetResetVersion())
{
}

renderer::ResourceManager* WaterRenderCache::FindLiveOwner() const
{
    auto* resources = renderer::ResourceManager::FindOwner(m_resourceOwner);
    return resources && resources->GetResetVersion() == m_resetVersion ? resources : nullptr;
}

void ReleaseWaterMesh(WaterMesh& mesh, renderer::ResourceManager& resources)
{
    for (auto& chunk : mesh.chunks) {
        if (chunk.vertexBuffer.IsValid()) resources.Release(chunk.vertexBuffer);
        if (chunk.indexBuffer.IsValid()) resources.Release(chunk.indexBuffer);
        if (chunk.strip.buffer.IsValid()) resources.Release(chunk.strip.buffer);
    }
    if (mesh.surfaceClusterIndices.IsValid()) resources.Release(mesh.surfaceClusterIndices);
    if (mesh.surfaceClusterVertices.IsValid()) resources.Release(mesh.surfaceClusterVertices);
    mesh = {};
}

template<class Erase>
void WaterRenderCache::ReleaseEntries(renderer::ResourceManager& resources, Erase erase)
{
    if (m_resourceOwner != resources.GetInstanceId() || m_resetVersion != resources.GetResetVersion()) return;
    erase(m_meshes, [&](WaterMesh& mesh) {
        ReleaseWaterMesh(mesh, resources);
    });
    erase(m_appearances, [&](renderer::Material& appearance) {
        if (appearance.paramsBuffer.IsValid()) resources.Release(appearance.paramsBuffer);
        appearance.paramsBuffer = {};
    });
    erase(m_textures, [&](const WaterTextures& textures) {
        if (textures.terrainInfoTexture.IsValid()) resources.Release(textures.terrainInfoTexture);
    });
    erase(m_ripples, [&](const WaterRippleState& ripple) {
        if (ripple.texture.IsValid()) resources.Release(ripple.texture);
    });
}

WaterRenderCache::~WaterRenderCache()
{
    if (auto* resources = FindLiveOwner()) {
        ReleaseEntries(*resources, [](auto& entries, auto release) {
            for (auto& entry : entries) release(entry.second);
            entries.clear();
        });
    }
    /// @note 終了済み・Reset 済みの資源は返さずに手放し、Material の破棄にも旧ハンドルを渡さない。
    for (auto& [key, appearance] : m_appearances) appearance.paramsBuffer = {};
}

WaterRenderCache& WaterRenderCache::Acquire(Scene& scene, renderer::ResourceManager& resources)
{
    auto& caches = s_sceneWaterCaches[scene.GetRenderSceneGeneration()];
    std::erase_if(caches, [](const auto& cache) { return !cache->FindLiveOwner(); });
    for (auto& cache : caches) if (cache->m_resourceOwner == resources.GetInstanceId()) return *cache;
    caches.push_back(std::make_unique<WaterRenderCache>(resources));
    return *caches.back();
}

/// @note フラグを消す前にシーンを描く全リソース管理元へ変更を伝える。
void WaterRenderCache::Invalidate(const WaterCacheKey& key, bool meshDirty, bool textureDirty)
{
    for (auto& cache : s_sceneWaterCaches.at(key.sceneGeneration)) {
        if (meshDirty) cache->m_dirtyMeshes.insert(key);
        if (textureDirty) cache->m_dirtyTextures.insert(key);
    }
}

void WaterRenderCache::ReleaseScene(Scene& scene)
{
    s_sceneWaterCaches.erase(scene.GetRenderSceneGeneration());
}

void WaterRenderCache::ReleaseEntity(Scene& scene, EntityID entity)
{
    const auto sceneCaches = s_sceneWaterCaches.find(scene.GetRenderSceneGeneration());
    if (sceneCaches == s_sceneWaterCaches.end()) return;
    const WaterCacheKey key{scene.GetRenderSceneGeneration(), entity.index, entity.generation};
    for (auto& cache : sceneCaches->second) {
        if (auto* resources = cache->FindLiveOwner()) {
            cache->ReleaseEntries(*resources, [&](auto& entries, auto release) {
                const auto entry = entries.find(key);
                if (entry == entries.end()) return;
                release(entry->second);
                entries.erase(entry);
            });
        }
        cache->m_dirtyMeshes.erase(key);
        cache->m_dirtyTextures.erase(key);
    }
}

void WaterRenderCache::ReleaseUnavailableBodies(Scene& scene, renderer::ResourceManager& resources)
{
    /// @note 水域の削除時は、単独の水域とゾーンに属する水域のどちらも専用の描画リソースを解放する。
    ReleaseEntries(resources, [&](auto& entries, auto release) {
        std::erase_if(entries, [&](auto& entry) {
            const auto& key = entry.first;
            const auto* water = scene.GetComponent<WaterComponent>({key.entityIndex, key.entityGeneration});
            if (water && !WaterSurfaceZoneOwner(*water, WaterSurfaceReadView::SIMULATION).IsValid()) return false;
            release(entry.second);
            return true;
        });
    });
}

bool WaterRenderCache::CollectTerrainInputs(Scene& scene, WaterTerrainInfoInputs& terrainInputs)
{
    FBZZ_PROFILE_SCOPE("WaterPrepare::TerrainSources");
    /// @note Terrain は抽出中だけ借用する。同じフレームの追加・削除・配列移動後も再取得する。
    return m_terrainInfo[scene.GetRenderSceneGeneration()].TryCollect(scene, terrainInputs);
}

void WaterRenderCache::PrepareMesh(const WaterCacheKey& key, WaterComponent& water,
    renderer::ResourceManager& resources, bool buildSurfaceClusters)
{
    const auto cachedMesh = m_meshes.find(key);
    const bool geometryChanged = cachedMesh != m_meshes.end()
        && (cachedMesh->second.geometry != ResolveWaterMeshGeometry(water)
            || (buildSurfaceClusters && !cachedMesh->second.surfaceClustersAttempted));
    if (m_dirtyMeshes.contains(key) || cachedMesh == m_meshes.end() || geometryChanged) {
        FBZZ_PROFILE_SCOPE("WaterPrepare::Mesh");
        BuildWaterMesh(water, m_meshes[key], m_patchGrids, resources, buildSurfaceClusters);
        m_dirtyMeshes.erase(key);
        m_dirtyTextures.insert(key);
        water.meshDirty = false;
        water.foamDirty = true;
    }
}

void WaterRenderCache::PrepareTerrainTextures(const WaterCacheKey& key, WaterComponent& water,
    const Transform& transform, const WaterTerrainInfoInputs& terrainInputs, const WaterTextureRequest& request,
    renderer::IRenderer& renderer, renderer::ResourceManager& resources)
{
    if (m_dirtyTextures.contains(key) || !m_textures.contains(key)
        || m_textures.at(key).signature != request.signature) {
        FBZZ_PROFILE_SCOPE("WaterPrepare::Textures");
        const bool replaceTexture = m_dirtyTextures.erase(key) != 0;
        WaterTextures& textures = m_textures[key];
        UpdateWaterTextures(
            textures, water, transform, terrainInputs, request, replaceTexture, renderer, resources);
        water.texDirty = false;
        water.foamDirty = false;
    }
}

const WaterRippleState& WaterRenderCache::PrepareRipples(const WaterCacheKey& key, const WaterComponent& water,
    const Transform& transform, renderer::ResourceManager& resources)
{
    /// @note 波紋がなく既存テクスチャも有効なら CPU 更新・GPU アップロードを省く。
    /// @note 最後の波紋が消えたフレームにも、中立値で焼き直す。
    WaterRippleState& rippleState = m_ripples[key];
    if (rippleState.dirty || !rippleState.texture.IsValid()
        || !water.ripples.empty() || rippleState.previousBakeHadRipples) {
        FBZZ_PROFILE_SCOPE("WaterPrepare::Ripples");
        UpdateRippleState(rippleState, water, transform, resources);
    }
    return rippleState;
}

}

namespace fbzz::scene {

void ReleaseWaterRenderResources(Scene& scene)
{
    water_render_detail::WaterRenderCache::ReleaseScene(scene);
}

void ReleaseWaterRenderResources(Scene& scene, EntityID entity)
{
    water_render_detail::WaterRenderCache::ReleaseEntity(scene, entity);
}

}
