/// @file    RenderTerrainExtractor.cpp
/// @brief   TerrainComponent から描画用パッチと GPU 入力を抽出する。
/// @author  Hasegawa Jin
/// @date    2026-06-18

/// @note 枠 (TerrainSurface.hlsli と同期): t0 = 層番号マップ、t1 = 重みマップ、t13 = 影。層テクスチャと層パラメーターは
/// @note StructuredBuffer (TerrainLayerGpu) と bindless 添字で渡す。
/// @note シングルスレッド前提。GPU 資源は Scene 世代・作成元・Reset 世代で分離したキャッシュが所有する。
/// @note heightDirty (穴を含む) でチャンクを、splatDirty で番号 / 重みマップを作り直す。
/// @see Docs/design/terrain-layers.md
#include <Engine/Asset/StreamedTextureResolver.hpp>
#include <Engine/Asset/BakeFingerprint.hpp>
#include "Engine/Scene/Systems/RenderPasses/Geometry/TerrainRenderPass.hpp"
#include "RenderPasses/Geometry/GeometryPasses.hpp"
#include <Engine/Scene/Systems/RenderTerrainExtractor.hpp>
#include "TerrainHeightHelpers.hpp"
#include "TerrainRenderCache.hpp"
#include "../../Asset/CacheableLoadedReference.hpp"
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Renderer/SurfacePatchLod.hpp>
#include "RenderPasses/Geometry/TerrainPatchIndices.hpp"
#include "RenderPasses/Geometry/TerrainSurfacePatch.hpp"
#include "Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp"
#include "Engine/Asset/AssetManager.hpp"
#include "Engine/Asset/MaterialAsset.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Transform.hpp"
#include "Engine/Scene/Components/TerrainComponent.hpp"
#include <Engine/Scene/TerrainSurfaceGeometry.hpp>
#include <Engine/Renderer/ITexture.hpp>
#include <Engine/Renderer/IStructuredBuffer.hpp>
#include <Engine/Scene/Components/FiberComponent.hpp>
#include "Engine/Scene/Components/TerrainGridComponent.hpp"
#include "Engine/Scene/Entity.hpp"
#include "Engine/Renderer/IRenderer.hpp"
#include "Engine/Renderer/Camera.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include "Engine/Renderer/DrawCall.hpp"
#include "Engine/Renderer/RenderSettings.hpp"
#include "Engine/Renderer/RenderState.hpp"
#include <Math/Frustum.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Physics/Layer.hpp>
#include <Core/Profiler/ProfileScope.hpp>
#include <array>
#include <algorithm>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <limits>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fbzz::scene {

using terrain_height_detail::TryAddTerrainHeightSample;

namespace {
/// @note Only an explicit terrain diagnostic reads clocks; accumulate phases without per-patch profiler records.
class TerrainPhaseTimer {
public:
    TerrainPhaseTimer(bool enabled, int64_t& elapsed) : m_elapsed(enabled ? &elapsed : nullptr)
    { if (m_elapsed) m_start = std::chrono::steady_clock::now(); }
    ~TerrainPhaseTimer() { Finish(); }
    void Finish()
    {
        if (!m_elapsed) return;
        *m_elapsed += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - m_start).count();
        m_elapsed = nullptr;
    }
private:
    int64_t* m_elapsed;
    std::chrono::steady_clock::time_point m_start{};
};

struct TerrainCandidatePool {
    std::vector<std::unique_ptr<std::vector<EntityID>>> entries;
    size_t depth = 0;
};

TerrainCandidatePool& TerrainCandidates()
{
    thread_local TerrainCandidatePool pool;
    return pool;
}

/// @note Nested extraction owns separate stable storage; release retains capacity without retaining Scene references.
class TerrainCandidateLease {
public:
    explicit TerrainCandidateLease(Scene& scene)
    {
        auto& pool = TerrainCandidates();
        if (pool.depth == pool.entries.size())
            pool.entries.push_back(std::make_unique<std::vector<EntityID>>());
        m_entities = pool.entries[pool.depth++].get();
        const auto source = scene.GetEntities<TerrainComponent>();
        m_entities->assign(source.begin(), source.end());
    }
    ~TerrainCandidateLease() { m_entities->clear(); --TerrainCandidates().depth; }
    TerrainCandidateLease(const TerrainCandidateLease&) = delete;
    TerrainCandidateLease& operator=(const TerrainCandidateLease&) = delete;
    std::span<const EntityID> Entities() const { return *m_entities; }
private:
    std::vector<EntityID>* m_entities = nullptr;
};
} /// @note namespace

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

static float SampleStitchedTerrainHeight(
    const TerrainComponent& terrain,
    const TerrainNeighbors& neighbors,
    int                     x,
    int                     z)
{
    /// @note TerrainGrid の境界では複数 Terrain が同じワールド頂点を持つ。描画用メッシュでは
    /// @note その共有頂点を平均し、保存データを破壊せずに隙間を隠す。角は最大 4 Terrain が
    /// @note 交差するため、斜め隣接も平均に含める。
    float sum = 0.0f;
    int count = 0;

    TryAddTerrainHeightSample(&terrain, x, z, sum, count);

    if (x <= 0) {
        TryAddTerrainHeightSample(neighbors.west, neighbors.west ? neighbors.west->columns - 1 + x : x, z, sum, count);
    }
    if (x >= terrain.columns - 1) {
        TryAddTerrainHeightSample(neighbors.east, x - (terrain.columns - 1), z, sum, count);
    }
    if (z <= 0) {
        TryAddTerrainHeightSample(neighbors.north, x, neighbors.north ? neighbors.north->rows - 1 + z : z, sum, count);
    }
    if (z >= terrain.rows - 1) {
        TryAddTerrainHeightSample(neighbors.south, x, z - (terrain.rows - 1), sum, count);
    }

    if (x <= 0 && z <= 0) {
        TryAddTerrainHeightSample(
            neighbors.northWest,
            neighbors.northWest ? neighbors.northWest->columns - 1 + x : x,
            neighbors.northWest ? neighbors.northWest->rows - 1 + z : z,
            sum,
            count);
    }
    if (x >= terrain.columns - 1 && z <= 0) {
        TryAddTerrainHeightSample(
            neighbors.northEast,
            x - (terrain.columns - 1),
            neighbors.northEast ? neighbors.northEast->rows - 1 + z : z,
            sum,
            count);
    }
    if (x <= 0 && z >= terrain.rows - 1) {
        TryAddTerrainHeightSample(
            neighbors.southWest,
            neighbors.southWest ? neighbors.southWest->columns - 1 + x : x,
            z - (terrain.rows - 1),
            sum,
            count);
    }
    if (x >= terrain.columns - 1 && z >= terrain.rows - 1) {
        TryAddTerrainHeightSample(
            neighbors.southEast,
            x - (terrain.columns - 1),
            z - (terrain.rows - 1),
            sum,
            count);
    }

    if (count > 0)
        return sum / static_cast<float>(count);

    const int clampedX = std::clamp(x, 0, terrain.columns - 1);
    const int clampedZ = std::clamp(z, 0, terrain.rows - 1);
    const size_t idx = static_cast<size_t>(clampedZ) * static_cast<size_t>(terrain.columns)
                     + static_cast<size_t>(clampedX);
    return terrain.heightData[idx] * terrain.maxHeight;
}

static math::Vector3 ComputeStitchedNormal(
    const TerrainComponent& terrain,
    const TerrainNeighbors& neighbors,
    int                     x,
    int                     z)
{
    /// @note 補正済み高さで中心差分を取り、頂点位置だけでなく陰影も隣接 Terrain と連続させる。
    /// @note `TerrainComponent::ComputeNormal()` は単体 Terrain 用に端を clamp するため、
    /// @note TerrainGrid では境界法線が隣の傾斜を見ず、継ぎ目が暗線として残ってしまう。
    const float dhdx = (SampleStitchedTerrainHeight(terrain, neighbors, x + 1, z)
                      - SampleStitchedTerrainHeight(terrain, neighbors, x - 1, z))
                     / (2.0f * terrain.cellSize);
    const float dhdz = (SampleStitchedTerrainHeight(terrain, neighbors, x, z + 1)
                      - SampleStitchedTerrainHeight(terrain, neighbors, x, z - 1))
                     / (2.0f * terrain.cellSize);
    return math::Vector3{ -dhdx, 1.0f, -dhdz }.Normalized();
}

static TerrainNeighbors ResolveTerrainNeighbors(
    Scene&                       scene,
    const TerrainGridComponent*  terrainGrid,
    EntityID                     eid)
{
    TerrainNeighbors neighbors;
    if (!terrainGrid)
        return neighbors;

    int gx = 0;
    int gz = 0;
    if (!terrainGrid->TryGetGridPos(eid, gx, gz))
        return neighbors;

    auto resolveNeighbor = [&](int ngx, int ngz) -> const TerrainComponent* {
        const EntityID neid = terrainGrid->GetCell(ngx, ngz);
        if (!scene.IsValid(neid))
            return nullptr;
        return scene.GetComponent<TerrainComponent>(neid);
    };

    neighbors.north     = resolveNeighbor(gx,     gz - 1);
    neighbors.south     = resolveNeighbor(gx,     gz + 1);
    neighbors.west      = resolveNeighbor(gx - 1, gz);
    neighbors.east      = resolveNeighbor(gx + 1, gz);
    neighbors.northWest = resolveNeighbor(gx - 1, gz - 1);
    neighbors.northEast = resolveNeighbor(gx + 1, gz - 1);
    neighbors.southWest = resolveNeighbor(gx - 1, gz + 1);
    neighbors.southEast = resolveNeighbor(gx + 1, gz + 1);
    return neighbors;
}

static void EraseTerrainChunkCache(TerrainRenderCache& cache, renderer::ResourceManager& resources, EntityID eid)
{
    std::erase_if(cache.chunks, [&](auto& kv) {
        if (kv.first.entityId != eid)
            return false;
        ReleaseTerrainChunkBuffers(resources, kv.second);
        return true;
    });
}

static void EraseTerrainChunkCacheForComponent(TerrainRenderCache& cache, Scene& scene, renderer::ResourceManager& resources, const TerrainComponent* terrain)
{
    if (!terrain)
        return;

    for (EntityID candidate : scene.GetEntities<TerrainComponent>()) {
        if (scene.GetComponent<TerrainComponent>(candidate) == terrain) {
            EraseTerrainChunkCache(cache, resources, candidate);
            return;
        }
    }
}

static void EraseNeighborTerrainChunkCaches(TerrainRenderCache& cache, Scene& scene, renderer::ResourceManager& resources, const TerrainNeighbors& neighbors)
{
    /// @note 境界頂点は隣接 Terrain の高さを参照して構築されるため、自身の高さが変わったときは
    /// @note 隣接側の GPU キャッシュも無効化する。heightDirty を相互に立てると隣接同士で毎フレーム
    /// @note 再 dirty 化されるため、ここではキャッシュだけを直接破棄する (GPU バッファは解放する)。
    EraseTerrainChunkCacheForComponent(cache, scene, resources, neighbors.north);
    EraseTerrainChunkCacheForComponent(cache, scene, resources, neighbors.south);
    EraseTerrainChunkCacheForComponent(cache, scene, resources, neighbors.west);
    EraseTerrainChunkCacheForComponent(cache, scene, resources, neighbors.east);
    EraseTerrainChunkCacheForComponent(cache, scene, resources, neighbors.northWest);
    EraseTerrainChunkCacheForComponent(cache, scene, resources, neighbors.northEast);
    EraseTerrainChunkCacheForComponent(cache, scene, resources, neighbors.southWest);
    EraseTerrainChunkCacheForComponent(cache, scene, resources, neighbors.southEast);
}

static void BuildChunk(
    const TerrainComponent&      terrain,
    int                          cx,
    int                          cz,
    const TerrainNeighbors&      neighbors,
    std::vector<TerrainVertex>&  outVerts,
    math::Vector3&               outAabbMin,
    math::Vector3&               outAabbMax,
    int&                         outX0,
    int&                         outZ0,
    int&                         outX1,
    int&                         outZ1)
{
    outX0 = cx * terrain.chunkSize;
    outZ0 = cz * terrain.chunkSize;
    outX1 = std::min(outX0 + terrain.chunkSize, terrain.columns - 1);
    outZ1 = std::min(outZ0 + terrain.chunkSize, terrain.rows    - 1);

    outAabbMin = {  1e30f,  1e30f,  1e30f };
    outAabbMax = { -1e30f, -1e30f, -1e30f };

    for (int z = outZ0; z <= outZ1; ++z) {
        for (int x = outX0; x <= outX1; ++x) {
            const float h = SampleStitchedTerrainHeight(terrain, neighbors, x, z);

            TerrainVertex v;
            v.position = {
                static_cast<float>(x) * terrain.cellSize,
                h,
                static_cast<float>(z) * terrain.cellSize
            };
            v.normal = ComputeStitchedNormal(terrain, neighbors, x, z);

            {
                const float hL = SampleStitchedTerrainHeight(terrain, neighbors, x - 1, z);
                const float hR = SampleStitchedTerrainHeight(terrain, neighbors, x + 1, z);
                const float span = 2.0f * terrain.cellSize;
                const float dhDx = (hR - hL) / span;
                v.tangent = math::Vector3{ terrain.cellSize, dhDx * terrain.cellSize, 0.0f }.Normalized();
            }

            v.uv = {
                static_cast<float>(x) / static_cast<float>(terrain.columns - 1),
                static_cast<float>(z) / static_cast<float>(terrain.rows    - 1)
            };
            outVerts.push_back(v);

            outAabbMin.x = std::min(outAabbMin.x, v.position.x);
            outAabbMin.y = std::min(outAabbMin.y, v.position.y);
            outAabbMin.z = std::min(outAabbMin.z, v.position.z);
            outAabbMax.x = std::max(outAabbMax.x, v.position.x);
            outAabbMax.y = std::max(outAabbMax.y, v.position.y);
            outAabbMax.z = std::max(outAabbMax.z, v.position.z);
        }
    }
}

static TerrainChunk& EnsureTerrainChunk(
    TerrainRenderCache& cache,
    const TerrainComponent& terrain,
    EntityID eid,
    int cx,
    int cz,
    const TerrainNeighbors& neighbors,
    size_t lodCount,
    renderer::ResourceManager& resources)
{
    const TerrainChunkKey key{ eid, static_cast<uint32_t>(cx), static_cast<uint32_t>(cz) };
    if (!cache.chunks.contains(key)) {
        std::vector<TerrainVertex> verts;
        math::Vector3 aabbMin, aabbMax;
        int x0, z0, x1, z1;
        BuildChunk(terrain, cx, cz, neighbors, verts, aabbMin, aabbMax, x0, z0, x1, z1);

        TerrainChunk chunk;
        chunk.gpuPatch = {static_cast<uint32_t>(x0), static_cast<uint32_t>(z0),
            static_cast<uint32_t>(x1 - x0 + 1), static_cast<uint32_t>(z1 - z0 + 1),
            static_cast<uint32_t>(terrain.columns), static_cast<uint32_t>(terrain.rows), terrain.cellSize, 0};
        chunk.aabbMin = aabbMin;
        chunk.aabbMax = aabbMax;
        chunk.vertexBuffer = resources.CreateVertexBuffer(
            verts.data(),
            verts.size() * sizeof(TerrainVertex),
            static_cast<uint32_t>(sizeof(TerrainVertex)));

        auto occlusion = verts.size() <= UINT16_MAX ? std::make_shared<renderer::TerrainOcclusionGeometry>() : nullptr;
        if (occlusion) {
            occlusion->heights.reserve(verts.size());
            for (const auto& vertex : verts) occlusion->heights.push_back(vertex.position.y);
        }

        for (size_t lod = 0; lod < lodCount; ++lod) {
            std::vector<uint32_t> indices;
            BuildTerrainPatchLodIndices(terrain, x0, z0, x1, z1, renderer::TERRAIN_LOD_STEPS[lod], indices);
            chunk.indexCountLOD[lod] = static_cast<uint32_t>(indices.size());
            /// @note 全セルが穴のチャンクはインデックス 0 本。空バッファは作らず、描画側で飛ばす。
            if (chunk.indexCountLOD[lod] > 0)
                chunk.indexBufferLOD[lod] = resources.CreateIndexBuffer(indices.data(), chunk.indexCountLOD[lod]);
            if (occlusion) {
                auto& cpuIndices = occlusion->indices[lod];
                cpuIndices.reserve(indices.size());
                for (const auto index : indices) cpuIndices.push_back(static_cast<uint16_t>(index));
            }
        }
        if (occlusion && chunk.indexCountLOD[0] == uint32_t((x1 - x0) * (z1 - z0) * 6)) {
            constexpr int columnStep = 16;
            /// @note 列の境界を最大 LOD の格子に揃え、内側へ張り出す三角形の頂点を高さ下限に含める。
            const int overlap = renderer::TERRAIN_LOD_STEPS[lodCount - 1];
            for (int z = z0; z < z1; z += columnStep) {
                for (int x = x0; x < x1; x += columnStep) {
                    const int lowX = std::max(x - overlap, 0), lowZ = std::max(z - overlap, 0);
                    const int highX = std::min(x + columnStep + overlap, terrain.columns - 1);
                    const int highZ = std::min(z + columnStep + overlap, terrain.rows - 1);
                    float minimum = std::numeric_limits<float>::infinity();
                    for (int sz = lowZ; sz <= highZ; ++sz)
                        for (int sx = lowX; sx <= highX; ++sx)
                            minimum = std::min(minimum, SampleStitchedTerrainHeight(terrain, neighbors, sx, sz));
                    occlusion->columns.push_back({{float(lowX) * terrain.cellSize, float(lowZ) * terrain.cellSize,
                        float(highX) * terrain.cellSize, float(highZ) * terrain.cellSize}, minimum});
                }
            }
        }
        chunk.occlusion = std::move(occlusion);
        cache.chunks[key] = std::move(chunk);
        cache.entities[eid.index].geometryDirty = true;
    }
    return cache.chunks.at(key);
}

static renderer::TerrainStripIndices EnsureTerrainStripIndices(TerrainGpuState& gpu, const TerrainChunk& chunk,
    uint32_t fullVertexCount, renderer::ResourceManager& resources)
{
    const auto& grid = chunk.gpuPatch;
    const uint64_t cellCount = static_cast<uint64_t>(grid.columns - 1) * (grid.rows - 1);
    /// @note LOD0 emits six indices for every solid cell, so fewer indices imply a hole in this grid.
    if (chunk.indexCountLOD[0] / 6u != cellCount) return {};

    const size_t slot = (grid.columns != fullVertexCount ? 1u : 0u) + (grid.rows != fullVertexCount ? 2u : 0u);
    auto& strip = gpu.rasterStrips[slot];
    if (strip.columns != grid.columns || strip.rows != grid.rows) {
        resources.Release(strip.indices.buffer);
        strip = {{}, grid.columns, grid.rows};
    }
    if (!strip.indices.buffer.IsValid()) {
        std::vector<uint32_t> indices;
        if (!renderer::BuildRegularSurfaceStripIndices(grid.columns - 1, grid.rows - 1, indices)) return {};
        strip.indices.count = static_cast<uint32_t>(indices.size());
        strip.indices.buffer = resources.CreateIndexBuffer(indices.data(), strip.indices.count);
    }
    return strip.indices;
}

static float TGetF(const asset::MaterialAssetAuthoring* m, const std::string& name, float def)
{
    if (!m) return def;
    auto it = m->params.find(name);
    if (it != m->params.end() && !it->second.empty()) return it->second[0];
    return def;
}

/// @note Retain handles, never MaterialAsset pointers or authoring snapshots; edits/reimports remain visible on every extraction.
/// @see Docs/design/instanced-mesh.md
static asset::MaterialAsset::AuthoringSnapshotPtr ReadTerrainMaterial(TerrainRenderCache& cache, const std::string& path)
{
    if (path.empty()) return {};
    const auto revision = asset::AssetDatabase::Revision();
    const auto generation = asset::AssetManager::GetAssetGeneration();
    if (cache.materialIndexRevision != revision || cache.materialAssetGeneration != generation) {
        cache.materials.clear(); cache.materialIndexRevision = revision; cache.materialAssetGeneration = generation;
    }
    if (const auto found = cache.materials.find(path); found != cache.materials.end()) {
        if (const auto* material = asset::AssetManager::Get(found->second)) return material->ReadAuthoring();
        cache.materials.erase(found);
    }
    const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(path);
    const auto* material = asset::AssetManager::Get(handle);
    if (material && cache.materials.size() < 4096 && asset::detail::IsCacheableLoadedReference(path))
        cache.materials.emplace(path, handle);
    return material ? material->ReadAuthoring() : asset::MaterialAsset::AuthoringSnapshotPtr{};
}
static std::string TGetTex(const asset::MaterialAssetAuthoring* m, const std::string& name)
{
    if (!m) return {};
    auto it = m->textures.find(name);
    if (it != m->textures.end()) return it->second;
    return {};
}

static TerrainLayerGpu ReadTerrainLayerParamsImpl(const asset::MaterialAssetAuthoring* material, std::string_view keyPrefix)
{
    const std::string prefix(keyPrefix);
    const auto get = [&](const char* key, float def) { return TGetF(material, prefix + key, def); };
    TerrainLayerGpu layer;
    layer.tilingX            = get("tilingX", 8.0f);
    layer.tilingZ            = get("tilingZ", 8.0f);
    layer.normalStrength     = get("normalStrength", 1.0f);
    layer.roughness          = get("roughness", 0.8f);
    layer.ambientOcclusion   = get("ambientOcclusion", 1.0f);
    layer.hasAoRoughness     = TGetTex(material, prefix + "ao_roughness").empty() ? 0.0f : 1.0f;
    layer.hasHeight          = TGetTex(material, prefix + "height").empty() ? 0.0f : 1.0f;
    layer.heightBlend        = get("heightBlend", 0.0f);
    layer.autoMinHeight      = get("autoMinHeight", -10000.0f);
    layer.autoMaxHeight      = get("autoMaxHeight", 10000.0f);
    layer.autoHeightFade     = get("autoHeightFade", 1.0f);
    layer.autoBlendEnabled   = get("autoBlendEnabled", 0.0f);
    layer.autoMinSlope       = get("autoMinSlope", 0.0f);
    layer.autoMaxSlope       = get("autoMaxSlope", 1.0f);
    layer.autoSlopeFade      = get("autoSlopeFade", 0.1f);
    layer.autoBlendStrength  = get("autoBlendStrength", 1.0f);
    layer.triplanar          = get("triplanar", 0.0f);
    layer.triplanarSharpness = get("triplanarSharpness", 4.0f);
    layer.macroScale         = get("macroScale", 0.1f);
    layer.macroStrength      = get("macroStrength", 0.0f);
    return layer;
}

TerrainLayerGpu ReadTerrainLayerParams(const asset::MaterialAssetAuthoring& material, std::string_view keyPrefix)
{
    return ReadTerrainLayerParamsImpl(&material, keyPrefix);
}

TerrainLayerGpu ReadTerrainLayerParams(const asset::MaterialAsset* material, std::string_view keyPrefix)
{
    const auto snapshot = material ? material->ReadAuthoring() : asset::MaterialAsset::AuthoringSnapshotPtr{};
    return ReadTerrainLayerParamsImpl(snapshot ? &snapshot->Data() : nullptr, keyPrefix);
}

bool TerrainFallbackTextures::IsValid() const
{
    return white.IsValid() && flatNormal.IsValid() && black.IsValid() && gray.IsValid();
}

TerrainFallbackTextures CreateTerrainFallbackTextures(renderer::ResourceManager& resources)
{
    TerrainFallbackTextures t;
    const uint8_t white[4]  = { 255, 255, 255, 255 };
    const uint8_t normal[4] = { 128, 128, 255, 255 };
    const uint8_t black[4]  = {   0,   0,   0, 255 };
    const uint8_t gray[4]   = { 128, 128, 128, 255 };
    t.white      = resources.CreateTexture(white, 1, 1);
    t.flatNormal = resources.CreateTexture(normal, 1, 1);
    t.black      = resources.CreateTexture(black, 1, 1);
    t.gray       = resources.CreateTexture(gray, 1, 1);
    return t;
}

void ReleaseTerrainFallbackTextures(renderer::ResourceManager& resources, TerrainFallbackTextures& textures)
{
    resources.Release(textures.white);
    resources.Release(textures.flatNormal);
    resources.Release(textures.black);
    resources.Release(textures.gray);
    textures = {};
}

/// @return テクスチャの bindless 添字。未ロード・発行失敗なら代替の添字。
static uint32_t BindlessIndexOr(renderer::ResourceManager& resources,
                                renderer::ResourceHandle<renderer::TextureTag> texture,
                                renderer::ResourceHandle<renderer::TextureTag> fallback)
{
    if (const renderer::ITexture* t = resources.Get(texture)) {
        const uint32_t index = t->GetBindlessIndex();
        if (index != renderer::INVALID_BINDLESS_INDEX) return index;
    }
    if (const renderer::ITexture* f = resources.Get(fallback))
        return f->GetBindlessIndex();
    return renderer::INVALID_BINDLESS_INDEX;
}

static void FillTerrainLayerTextureIndicesImpl(renderer::ResourceManager& resources,
                                    const asset::MaterialAssetAuthoring* material,
                                    std::string_view keyPrefix,
                                    const TerrainFallbackTextures& fallback,
                                    TerrainLayerGpu& layer)
{
    const std::string prefix(keyPrefix);
    const auto load = [&](const char* key) {
        const std::string path = TGetTex(material, prefix + key);
        return path.empty() ? renderer::ResourceHandle<renderer::TextureTag>{} : asset::StreamedTextureResolver::Engine().ResolveGpu(resources, path);
    };
    layer.diffuseIndex     = BindlessIndexOr(resources, load("diffuse"), fallback.white);
    layer.normalIndex      = BindlessIndexOr(resources, load("normal"), fallback.flatNormal);
    layer.aoRoughnessIndex = BindlessIndexOr(resources, load("ao_roughness"), fallback.black);
    layer.heightIndex      = BindlessIndexOr(resources, load("height"), fallback.gray);
}

void FillTerrainLayerTextureIndices(renderer::ResourceManager& resources,
                                    const asset::MaterialAssetAuthoring& material,
                                    std::string_view keyPrefix,
                                    const TerrainFallbackTextures& fallback,
                                    TerrainLayerGpu& layer)
{
    FillTerrainLayerTextureIndicesImpl(resources, &material, keyPrefix, fallback, layer);
}

void FillTerrainLayerTextureIndices(renderer::ResourceManager& resources,
                                    const asset::MaterialAsset* material,
                                    std::string_view keyPrefix,
                                    const TerrainFallbackTextures& fallback,
                                    TerrainLayerGpu& layer)
{
    const auto snapshot = material ? material->ReadAuthoring() : asset::MaterialAsset::AuthoringSnapshotPtr{};
    FillTerrainLayerTextureIndicesImpl(resources, snapshot ? &snapshot->Data() : nullptr, keyPrefix, fallback, layer);
}

/// @brief 番号 / 重みマップを作り直す。どちらも頂点 1 つ = RGBA8 1 画素なので配列をそのまま渡せる。
static void RebuildSplatTextures(TerrainGpuState& state, const TerrainComponent& terrain,
                                 renderer::ResourceManager& resources)
{
    resources.Release(state.splatIndices);
    resources.Release(state.splatWeights);
    state.splatIndices = {};
    state.splatWeights = {};
    if (terrain.HasValidSplat()) {
        state.splatIndices = resources.CreateTexture(terrain.splatIndices.data(),
            static_cast<uint32_t>(terrain.columns), static_cast<uint32_t>(terrain.rows));
        state.splatWeights = resources.CreateTexture(terrain.splatWeights.data(),
            static_cast<uint32_t>(terrain.columns), static_cast<uint32_t>(terrain.rows));
        return;
    }
    /// @note スプラット未初期化の地形は «全頂点が層 0» の 1x1 で描く。
    const uint8_t indices[4] = { 0, 0, 0, 0 };
    const uint8_t weights[4] = { 255, 0, 0, 0 };
    state.splatIndices = resources.CreateTexture(indices, 1, 1);
    state.splatWeights = resources.CreateTexture(weights, 1, 1);
}

/// @note LOD0 の水平位置と索引は同じ格子、穴、パッチ範囲から生成する。高さは接合後の描画値を使う。
static uint64_t TerrainShadowBakeGeometryHash(const TerrainComponent& terrain,
    const std::vector<renderer::RenderTerrainPatch>& patches)
{
    asset::BakeFingerprint hash;
    hash.Add(static_cast<uint64_t>(patches.size()));
    hash.Add(static_cast<uint64_t>(terrain.holeData.size()));
    hash.Add(std::span<const uint8_t>(terrain.holeData));
    for (const auto& patch : patches) {
        if (!patch.occlusion || patch.occlusion->indices[0].size() != patch.indexCountLOD[0]) return 0;
        const auto& grid = patch.gpuPatch;
        for (uint32_t value : {grid.originX, grid.originZ, grid.columns, grid.rows,
            grid.terrainColumns, grid.terrainRows, patch.indexCountLOD[0]})
            hash.Add(static_cast<uint64_t>(value));
        hash.Add(std::span<const float>(&grid.cellSize, 1));
        hash.Add(std::span<const float>(patch.occlusion->heights));
    }
    const std::string text = hash.Finish();
    uint64_t value = 0;
    std::from_chars(text.data(), text.data() + text.size(), value, 16);
    return value;
}

/// @note Scalar samples include stitched neighbor heights; GPU central differences match the CPU fallback.
static void PrepareTerrainGpuGeometry(TerrainGpuState& state, const TerrainComponent& terrain,
    const TerrainNeighbors& neighbors, renderer::RenderTerrainInput& input,
    renderer::ResourceManager& resources)
{
    if (input.patches.empty()) return;
    if (state.geometryDirty) {
        state.shadowBakeGeometryHash = 0;
        resources.Release(state.heights);
        resources.Release(state.patches);
        resources.Release(state.indices);
        state.heights = {}; state.patches = {}; state.indices = {};
        state.indexStarts.clear();
        state.indexCount = 0; state.vertexCapacity = 0;
        std::vector<float> heights;
        heights.reserve(static_cast<size_t>(terrain.columns + 2) * static_cast<size_t>(terrain.rows + 2));
        for (int z = -1; z <= terrain.rows; ++z)
            for (int x = -1; x <= terrain.columns; ++x)
                heights.push_back(SampleStitchedTerrainHeight(terrain, neighbors, x, z));
        std::vector<renderer::TerrainGpuPatch> patches;
        std::vector<uint32_t> indices;
        for (const auto& chunk : input.patches) {
            const auto& patch = chunk.gpuPatch;
            patches.push_back(patch);
            decltype(chunk.gpuStartIndexLOD) starts{};
            for (size_t lod = 0; lod < input.LODCount(); ++lod) {
                starts[lod] = static_cast<uint32_t>(indices.size());
                BuildTerrainPatchLodIndices(terrain, static_cast<int>(patch.originX), static_cast<int>(patch.originZ),
                    static_cast<int>(patch.originX + patch.columns - 1),
                    static_cast<int>(patch.originZ + patch.rows - 1), renderer::TERRAIN_LOD_STEPS[lod], indices);
            }
            state.indexStarts.push_back(starts);
            state.vertexCapacity = std::max(state.vertexCapacity, patch.columns * patch.rows);
        }
        state.heights = resources.CreateStructuredBuffer(heights.data(), static_cast<uint32_t>(heights.size()), sizeof(float));
        state.patches = resources.CreateStructuredBuffer(patches.data(), static_cast<uint32_t>(patches.size()), sizeof(renderer::TerrainGpuPatch));
        state.indexCount = static_cast<uint32_t>(indices.size());
        state.indices = resources.CreateIndexBuffer(indices.data(), state.indexCount);
        state.geometryDirty = !resources.Get(state.heights) || !resources.Get(state.patches) || !resources.Get(state.indices);
    }
    if (state.geometryDirty || state.indexStarts.size() != input.patches.size()) return;
    for (size_t i = 0; i < input.patches.size(); ++i)
        input.patches[i].gpuStartIndexLOD = state.indexStarts[i];
    input.gpuHeights = state.heights;
    input.gpuPatches = state.patches;
    input.gpuIndices = state.indices;
    input.gpuIndexCount = state.indexCount;
    input.gpuVertexCapacity = state.vertexCapacity;
}

/// @brief 層配列を StructuredBuffer へ置く。層数が変わったら作り直し、内容が同じなら転送しない。
static void UploadLayerBuffer(TerrainGpuState& state, const std::vector<TerrainLayerGpu>& layers,
                              renderer::ResourceManager& resources)
{
    const uint32_t count = static_cast<uint32_t>(layers.size());
    const size_t bytes = layers.size() * sizeof(TerrainLayerGpu);
    if (!state.layerBuffer.IsValid() || state.layerCapacity != count) {
        resources.Release(state.layerBuffer);
        state.layerBuffer = resources.CreateStructuredBuffer(layers.data(), count,
                                                             static_cast<uint32_t>(sizeof(TerrainLayerGpu)));
        state.layerCapacity = count;
        state.uploadedLayers = layers;
        return;
    }
    if (state.uploadedLayers.size() == layers.size()
        && std::memcmp(state.uploadedLayers.data(), layers.data(), bytes) == 0)
        return;
    resources.Update(state.layerBuffer, layers.data(), bytes);
    state.uploadedLayers = layers;
}


/// @note Surface patches retain full topology at every LOD so entrances, walls and thin roofs never disappear.
static TerrainChunk& EnsureSurfaceChunk(TerrainRenderCache& cache, const TerrainComponent& terrain, EntityID eid, size_t first,
    renderer::ResourceManager& resources)
{
    const TerrainChunkKey key{eid, UINT32_MAX, static_cast<uint32_t>(first / TERRAIN_SURFACE_PATCH_INDICES)};
    const auto found = cache.chunks.find(key);
    if (found != cache.chunks.end()) return found->second;
    const auto data = BuildTerrainSurfacePatch(terrain, first);
    TerrainChunk chunk;
    chunk.aabbMin = data.aabbMin; chunk.aabbMax = data.aabbMax;
    chunk.vertexBuffer = resources.CreateVertexBuffer(data.vertices.data(), data.vertices.size() * sizeof(TerrainVertex), sizeof(TerrainVertex));
    for (size_t lod = 0; lod < renderer::TERRAIN_FAR_LOD_INDEX; ++lod) {
        chunk.indexCountLOD[lod] = static_cast<uint32_t>(data.indices.size());
        chunk.indexBufferLOD[lod] = resources.CreateIndexBuffer(data.indices.data(), chunk.indexCountLOD[lod]);
    }
    return cache.chunks.emplace(key, std::move(chunk)).first->second;
}
static void ExtractTerrainSurface(TerrainRenderCache& cache, const TerrainComponent& terrain, EntityID eid, TerrainGpuState& gpu,
    const renderer::RenderTerrainInput& base, const TerrainFallbackTextures& fallback,
    renderer::ResourceManager& resources, renderer::RenderScene& output)
{
    if (terrain.surfaceIndices.empty() || !IsValidTerrainSurfaceGeometry(terrain.surfacePositions, terrain.surfaceIndices)) return;
    renderer::RenderTerrainInput surface;
    surface.layer = base.layer; surface.selected = base.selected;
    surface.chunkWorldSize = base.chunkWorldSize;
    surface.constants = base.constants;
    surface.constants.grassGroundColor = {};
    surface.splatIndices = base.splatIndices; surface.splatWeights = base.splatWeights;
    if (!terrain.surfaceMaterial.empty()) {
        const auto materialAuthoringSnapshot = ReadTerrainMaterial(cache, terrain.surfaceMaterial);
        const auto* material = materialAuthoringSnapshot ? &materialAuthoringSnapshot->Data() : nullptr;
        auto layer = ReadTerrainLayerParamsImpl(material, {});
        layer.triplanar = 1.0f; layer.autoBlendEnabled = 0.0f;
        FillTerrainLayerTextureIndicesImpl(resources, material, {}, fallback, layer);
        if (!gpu.surfaceLayerBuffer.IsValid())
            gpu.surfaceLayerBuffer = resources.CreateStructuredBuffer(&layer, 1, sizeof(layer));
        else if (!gpu.uploadedSurfaceLayer || std::memcmp(&*gpu.uploadedSurfaceLayer, &layer, sizeof(layer)) != 0)
            resources.Update(gpu.surfaceLayerBuffer, &layer, sizeof(layer));
        gpu.uploadedSurfaceLayer = layer;
        if (!gpu.surfaceSplatIndices.IsValid()) {
            const uint8_t indices[4]{}, weights[4]{255, 0, 0, 0};
            gpu.surfaceSplatIndices = resources.CreateTexture(indices, 1, 1);
            gpu.surfaceSplatWeights = resources.CreateTexture(weights, 1, 1);
        }
        const auto* buffer = resources.Get(gpu.surfaceLayerBuffer);
        if (!buffer || buffer->GetBindlessIndex() == renderer::INVALID_BINDLESS_INDEX) return;
        surface.constants.layerBufferIndex = buffer->GetBindlessIndex();
        surface.constants.layerCount = 1;
        surface.constants.splatColumns = surface.constants.splatRows = 1;
        surface.constants.terrainParams.w = 0;
        surface.splatIndices = gpu.surfaceSplatIndices; surface.splatWeights = gpu.surfaceSplatWeights;
    }
    for (size_t first = 0; first < terrain.surfaceIndices.size(); first += TERRAIN_SURFACE_PATCH_INDICES)
        surface.patches.push_back(EnsureSurfaceChunk(cache, terrain, eid, first, resources));
    /// @note No GPU heightfield decoder and no terrain grass on cave ceilings; camera/shadow/mask consume these same patches.
    output.terrains.push_back(std::move(surface));
}

void ExtractRenderTerrains(RenderPassContext& ctx, renderer::RenderScene& output) {
    auto& scene = ctx.scene;
    auto& resources = ctx.resources;
    auto& cache = AcquireTerrainRenderCache(scene, resources);
    {
        std::unordered_set<uint32_t> validIndices;
        for (EntityID eid : scene.GetEntities<TerrainComponent>())
            validIndices.insert(eid.index);

        std::erase_if(cache.chunks, [&scene, &validIndices, &resources](auto& kv) {
            if (scene.IsValid(kv.first.entityId) && validIndices.count(kv.first.entityId.index)) return false;
            resources.Release(kv.second.vertexBuffer);
            for (auto& ib : kv.second.indexBufferLOD)
                resources.Release(ib);
            return true;
        });
        std::erase_if(cache.entities, [&validIndices, &resources](auto& kv) {
            if (validIndices.count(kv.first)) return false;
            ReleaseTerrainGpuState(kv.second, resources);
            return true;
        });
    }
    TerrainGridComponent* terrainGrid = nullptr;
    /// @note 親ごと無効化された Grid は使わない (有効な最初の 1 つ)。
    for (const EntityID gridId : scene.GetEntities<TerrainGridComponent>()) {
        const GameObject* gridObject = scene.GetGameObject(gridId);
        if (!gridObject || !gridObject->activeInHierarchy()) continue;
        terrainGrid = scene.GetComponent<TerrainGridComponent>(gridId);
        break;
    }

    /// @note 天候はシーンに 1 つ。TerrainGrid では地形が何十個も回るのでループの外で引く。
    const ActiveWeather weather = FindActiveWeather(scene);

    const TerrainCandidateLease candidates(scene);
    bool collectPhases = false;
    if (profiler::Profiler::IsEnabled()) {
        const auto filter = profiler::Profiler::GetRequestedCaptureFilter();
        collectPhases = filter.scope == "RenderSceneExtractor::Terrains"
            && (filter.category.empty() || filter.category == "General");
    }
    int64_t layersNs = 0, patchesNs = 0, setupNs = 0;
    for (const EntityID eid : candidates.Entities()) {
        TerrainPhaseTimer setupTimer(collectPhases, setupNs);
        auto* ownerObject = scene.GetGameObject(eid);
        auto* terrainComponent = scene.GetComponent<TerrainComponent>(eid);
        if (!ownerObject || !terrainComponent) continue;
        auto& terrain = *terrainComponent;
        const auto& transform = ownerObject->transform;
        if (!terrain.enabled || terrain.heightData.empty()) continue;

        assert(terrain.heightData.size() == static_cast<size_t>(terrain.columns)
                                          * static_cast<size_t>(terrain.rows));
        assert(terrain.chunkSize > 0);

        if (!ownerObject->activeInHierarchy()) continue;

        renderer::RenderTerrainInput input;
        if (std::isfinite(terrain.farLodDistance))
            input.farLodDistance = std::max(terrain.farLodDistance, 0.0f);
        const size_t lodCount = input.LODCount();
        const TerrainNeighbors neighbors = ResolveTerrainNeighbors(scene, terrainGrid, eid);

        TerrainGpuState& gpu = cache.entities[eid.index];
        if (gpu.entityId != eid) {
            ReleaseTerrainGpuState(gpu, resources);
            gpu.entityId = eid;
        }
        if (terrain.heightDirty) { ++terrain.surfaceRevision; terrain.heightDirty = false; }
        const bool surfaceChanged = gpu.surfaceRevision != terrain.surfaceRevision;
        if (surfaceChanged || gpu.lodCount != lodCount) {
            EraseTerrainChunkCache(cache, resources, eid);
            if (surfaceChanged) EraseNeighborTerrainChunkCaches(cache, scene, resources, neighbors);
            gpu.geometryDirty = true;
            gpu.surfaceRevision = terrain.surfaceRevision;
            gpu.lodCount = lodCount;
        }
        if (terrain.splatDirty) { ++terrain.vegetationRevision; terrain.splatDirty = false; }
        if (gpu.vegetationRevision != terrain.vegetationRevision || !gpu.splatIndices.IsValid() || !gpu.splatWeights.IsValid()) {
            RebuildSplatTextures(gpu, terrain, resources);
            gpu.vegetationRevision = terrain.vegetationRevision;
        }
        setupTimer.Finish();
        TerrainPhaseTimer layersTimer(collectPhases, layersNs);

        /// @note 地形の dirty に依存せず現 authoring を毎回読む。索引付きの読込済み参照だけ handle を再利用する。
        /// @note bindless 添字も毎フレーム引く。差し替え・ホットリロードで変わるため (Material::Upload と同じ方針)。
        std::vector<TerrainLayerGpu> layers;
        layers.reserve(static_cast<size_t>((std::max)(terrain.LayerCount(), 1)));
        bool anyAutoBlend = false;
        for (const std::string& path : terrain.layerMaterials) {
            const auto materialAuthoringSnapshot = ReadTerrainMaterial(cache, path);
            const auto* material = materialAuthoringSnapshot ? &materialAuthoringSnapshot->Data() : nullptr;
            TerrainLayerGpu layer = ReadTerrainLayerParamsImpl(material, {});
            FillTerrainLayerTextureIndicesImpl(resources, material, {}, cache.fallback, layer);
            anyAutoBlend |= layer.autoBlendEnabled > 0.0f && layer.autoBlendStrength > 0.0f;
            layers.push_back(layer);
        }
        if (layers.empty()) {
            /// @note 層 0 枚の地形は白い既定層 1 枚で描く。シェーダーは layerCount >= 1 を前提にする。
            TerrainLayerGpu layer;
            FillTerrainLayerTextureIndices(resources, nullptr, {}, cache.fallback, layer);
            layers.push_back(layer);
        }
        UploadLayerBuffer(gpu, layers, resources);
        terrain.materialParamDirty = false;

        const renderer::IStructuredBuffer* layerBuffer = resources.Get(gpu.layerBuffer);
        const uint32_t layerBufferIndex = layerBuffer ? layerBuffer->GetBindlessIndex() : renderer::INVALID_BINDLESS_INDEX;
        layersTimer.Finish();
        TerrainPhaseTimer patchesTimer(collectPhases, patchesNs);
        /// @note 添字が無いと ResourceDescriptorHeap の範囲外を読む。描かない方が原因を切り分けやすい。
        if (layerBufferIndex == renderer::INVALID_BINDLESS_INDEX) continue;

        const int chunkCountX = (terrain.columns - 1 + terrain.chunkSize - 1) / terrain.chunkSize;
        const int chunkCountZ = (terrain.rows    - 1 + terrain.chunkSize - 1) / terrain.chunkSize;

        const math::Matrix4 world = transform.GetWorldMatrix();
        TerrainObjectCB terrainCBData{};
        terrainCBData.worldMatrix = world;
        terrainCBData.worldInvTranspose = math::Matrix4::InverseTransposeAffine(world);
        terrainCBData.wvpMatrix   = world;
        terrainCBData.weather = { weather.wetness, weather.darkening, weather.puddleAmount, 0.0f };
        terrainCBData.terrainParams = {
            static_cast<float>(terrain.columns - 1) * terrain.cellSize,
            static_cast<float>(terrain.rows - 1) * terrain.cellSize,
            terrain.heightBlendDepth,
            anyAutoBlend ? 1.0f : 0.0f };
        terrainCBData.layerBufferIndex = layerBufferIndex;
        terrainCBData.layerCount       = static_cast<uint32_t>(layers.size());
        terrainCBData.splatColumns     = terrain.HasValidSplat() ? static_cast<uint32_t>(terrain.columns) : 1u;
        terrainCBData.splatRows        = terrain.HasValidSplat() ? static_cast<uint32_t>(terrain.rows) : 1u;

        input.layer = static_cast<uint32_t>(scene.GetGameObject(eid)->layer);
        input.constants = terrainCBData;
        input.splatIndices = gpu.splatIndices;
        input.splatWeights = gpu.splatWeights;
        input.chunkWorldSize = static_cast<float>(terrain.chunkSize) * terrain.cellSize;
        const auto* fiber = scene.GetComponent<FiberComponent>(eid);
        input.fiberSurface = fiber && fiber->m_enabled && !fiber->m_materialPath.empty();
        if (input.fiberSurface) {
            input.fiberDistanceLod = fiber->m_distanceLod;
            input.fiberLodFar = std::isfinite(fiber->m_lodFar) ? std::max(fiber->m_lodFar,0.1f) : 40.0f;
            const auto& color = fiber->m_terrainGroundColor;
            if (fiber->m_mode == FiberRenderMode::BLADE && fiber->m_distanceLod
                && fiber->m_bladeDensity > 0.0f && fiber->m_densityScale > 0.0f && fiber->m_lengthScale > 0.0f
                && fiber->m_terrainLayer < static_cast<int>(layers.size())
                && std::isfinite(fiber->m_lodNear) && std::isfinite(fiber->m_lodFar)
                && std::isfinite(fiber->m_terrainGroundCover) && fiber->m_terrainGroundCover > 0.0f
                && std::isfinite(color.x) && std::isfinite(color.y) && std::isfinite(color.z)) {
                const float nearDistance = std::max(fiber->m_lodNear, 0.0f);
                const float farDistance = std::max(fiber->m_lodFar, nearDistance + 0.1f);
                const float sparseStart = fiber->m_terrainSparseLodStart;
                const float fullDistance = sparseStart > nearDistance && sparseStart < farDistance ? sparseStart : farDistance;
                const float threshold = std::isfinite(fiber->m_terrainLayerThreshold)
                    ? std::clamp(fiber->m_terrainLayerThreshold, 0.0f, 1.0f) : 0.0f;
                input.constants.grassGroundLod = {nearDistance, fullDistance, static_cast<float>(fiber->m_terrainLayer), threshold};
                input.constants.grassGroundColor = {std::clamp(color.x, 0.0f, 1.0f), std::clamp(color.y, 0.0f, 1.0f),
                    std::clamp(color.z, 0.0f, 1.0f), std::clamp(fiber->m_terrainGroundCover, 0.0f, 1.0f)};
            }
        }
        for (const auto& selected : ctx.settings.selectedObjects)
            if (selected.index == eid.index && selected.generation == eid.generation) { input.selected = true; break; }
        const bool useRasterStrip = ctx.settings.rasterGeometryMode == renderer::RasterGeometryMode::TRIANGLE_STRIP;
        for (int cz = 0; cz < chunkCountZ; ++cz) {
            for (int cx = 0; cx < chunkCountX; ++cx) {
                const auto& chunk = EnsureTerrainChunk(cache, terrain, eid, cx, cz, neighbors, lodCount, resources);
                if (chunk.indexCountLOD[0] == 0) continue;
                auto& patch = input.patches.emplace_back(chunk);
                if (useRasterStrip) patch.lod0Strip = EnsureTerrainStripIndices(gpu, chunk,
                    static_cast<uint32_t>(terrain.chunkSize) + 1u, resources);
            }
        }
        PrepareTerrainGpuGeometry(gpu, terrain, neighbors, input, resources);
        if (ctx.directionalShadowBakeEnabled && !input.patches.empty()) {
            if (!gpu.shadowBakeGeometryHash)
                gpu.shadowBakeGeometryHash = TerrainShadowBakeGeometryHash(terrain, input.patches);
            input.shadowBakeGeometryHash = gpu.shadowBakeGeometryHash;
            input.shadowBakeStatic = input.shadowBakeGeometryHash != 0;
        }
        ExtractTerrainSurface(cache, terrain, eid, gpu, input, cache.fallback, resources, output);
        output.terrains.push_back(std::move(input));
    }
    if (collectPhases) {
        profiler::Profiler::PushSample({"RenderSceneExtractor::Terrains::Setup", "General"}, static_cast<double>(setupNs) / 1000000.0);
        profiler::Profiler::PushSample({"RenderSceneExtractor::Terrains::Layers", "General"}, static_cast<double>(layersNs) / 1000000.0);
        profiler::Profiler::PushSample({"RenderSceneExtractor::Terrains::Patches", "General"}, static_cast<double>(patchesNs) / 1000000.0);
    }
}
}
