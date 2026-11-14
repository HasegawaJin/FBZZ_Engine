/// @file    RenderTerrainExtractor.cpp
/// @brief   TerrainComponent から描画用パッチと GPU 入力を抽出する。
/// @author  Hasegawa Jin
/// @date    2026-06-18

/// @note 枠 (TerrainSurface.hlsli と同期): t0 = 層番号マップ、t1 = 重みマップ、t13 = 影。層テクスチャと層パラメーターは
/// @note StructuredBuffer (TerrainLayerGpu) と bindless 添字で渡す。
/// @note シングルスレッド前提。GPU 資源は Scene 世代・作成元・Reset 世代で分離したキャッシュが所有する。
/// @note 高さだけのプレビューは依存するチャンクを更新し、穴などの全体変更はトポロジーも再生成する。
/// @see Docs/design/terrain-layers.md
#include <Engine/Asset/StreamedTextureResolver.hpp>
#include <Engine/Asset/BakeFingerprint.hpp>
#include "Engine/Scene/Systems/RenderPasses/Geometry/TerrainRenderPass.hpp"
#include "RenderPasses/Geometry/GeometryPasses.hpp"
#include "RenderPasses/Debug/SelectionPasses.hpp"
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
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/WaterExclusion.hpp>
#include <Engine/Scene/WaterSurfaceQuery.hpp>
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

/// @note Scalar samples include stitched neighbor heights; GPU central differences match the CPU fallback.
static void FillTerrainHeightRow(const TerrainComponent& terrain, const TerrainNeighbors& neighbors,
    int firstX, int lastX, int z, float* output, bool canonicalZero = false)
{
    const auto sample = [&](int x) { output[x - firstX] = SampleStitchedTerrainHeight(terrain, neighbors, x, z); };
    if (z <= 0 || z >= terrain.rows - 1) {
        for (int x = firstX; x <= lastX; ++x) sample(x);
        return;
    }
    for (int x = firstX; x <= (std::min)(lastX, 0); ++x) sample(x);
    const int begin = (std::max)(firstX, 1), end = (std::min)(lastX, terrain.columns - 2);
    if (begin <= end) {
        const auto* source = terrain.heightData.data() + static_cast<size_t>(z) * terrain.columns;
        /// @note The CPU patch retains the stitched accumulator's signed-zero convention.
        if (canonicalZero) {
            std::transform(source + begin, source + end + 1, output + (begin - firstX),
                [maxHeight = terrain.maxHeight](float height) { return 0.0f + height * maxHeight; });
        } else {
            std::transform(source + begin, source + end + 1, output + (begin - firstX),
                [maxHeight = terrain.maxHeight](float height) { return height * maxHeight; });
        }
    }
    for (int x = (std::max)(firstX, terrain.columns - 1); x <= lastX; ++x) sample(x);
}

struct TerrainGridLookup {
    explicit TerrainGridLookup(const TerrainGridComponent* terrainGrid) : grid(terrainGrid)
    {
        if (!grid || grid->cellCountX <= 0 || grid->cellCountZ <= 0) return;
        const size_t width = static_cast<size_t>(grid->cellCountX);
        const size_t count = std::min(grid->cells.size(), width * static_cast<size_t>(grid->cellCountZ));
        m_positions.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const auto id = grid->cells[i];
            /// @note 重複参照は TryGetGridPos と同じく行優先で最初のセルを使う。
            if (id.IsValid()) m_positions.try_emplace(Key(id),
                std::array{static_cast<int>(i % width), static_cast<int>(i / width)});
        }
    }

    const std::array<int, 2>* Find(EntityID id) const
    {
        const auto found = m_positions.find(Key(id));
        return found == m_positions.end() ? nullptr : &found->second;
    }

    const TerrainGridComponent* grid;

private:
    static uint64_t Key(EntityID id) { return (uint64_t(id.generation) << 32) | id.index; }
    std::unordered_map<uint64_t, std::array<int, 2>> m_positions;
};

static TerrainNeighbors ResolveTerrainNeighbors(
    Scene&                       scene,
    const TerrainGridLookup&     gridLookup,
    EntityID                     eid)
{
    TerrainNeighbors neighbors;
    const auto* position = gridLookup.Find(eid);
    if (!position) return neighbors;
    const auto [gx, gz] = *position;

    auto resolveNeighbor = [&](int ngx, int ngz) -> const TerrainComponent* {
        const EntityID neid = gridLookup.grid->GetCell(ngx, ngz);
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

static void EraseTerrainChunkCache(TerrainGpuState& gpu, renderer::ResourceManager& resources)
{
    gpu.geometryDirty = true;
    gpu.heightsDirty = true;
    gpu.heightUploadRegion = {};
    gpu.shadowBakeGeometryHash = 0;
    ReleaseTerrainChunks(gpu, resources);
}

static void InvalidatePreviewChunks(renderer::ResourceManager& resources,
    const TerrainComponent& terrain, TerrainGpuState& gpu, const TerrainHeightRegion& region,
    bool heightChanged, bool topologyChanged = false)
{
    if (terrain.chunkSize <= 0 || terrain.columns < 2 || terrain.rows < 2) {
        EraseTerrainChunkCache(gpu, resources);
        return;
    }
    /// @note 遮蔽証明の高さ下限は法線より広い最大 LOD 幅を読む。境界頂点は両チャンクに含まれる。
    const int halo = heightChanged ? renderer::TERRAIN_LOD_STEPS[gpu.lodCount - 1] + 1 : 0;
    const int firstX = std::max(region.minX - halo, 0) / terrain.chunkSize;
    const int firstZ = std::max(region.minZ - halo, 0) / terrain.chunkSize;
    const int lastX = std::min(region.maxX + halo - 1, terrain.columns - 2) / terrain.chunkSize;
    const int lastZ = std::min(region.maxZ + halo - 1, terrain.rows - 2) / terrain.chunkSize;
    for (int z = firstZ; z <= lastZ; ++z) {
        for (int x = firstX; x <= lastX; ++x) {
            const auto chunk = gpu.chunks.find({static_cast<uint32_t>(x), static_cast<uint32_t>(z)});
            if (chunk == gpu.chunks.end()) continue;
            chunk->second.verticesDirty |= heightChanged;
            /// @note Hole rectangles include the far vertex of the last cell. LOD hole probes stay within each patch.
            chunk->second.topologyDirty |= topologyChanged
                && x * terrain.chunkSize < region.maxX && (x + 1) * terrain.chunkSize > region.minX
                && z * terrain.chunkSize < region.maxZ && (z + 1) * terrain.chunkSize > region.minZ;
        }
    }
    gpu.geometryDirty |= topologyChanged;
    gpu.shadowBakeGeometryHash = 0;
    if (!heightChanged) return;
    /// @note The padded height buffer also reads clamped own-tile edge samples when a neighbor is absent.
    TerrainHeightRegion padded = region;
    if (padded.minX == 0) --padded.minX;
    if (padded.minZ == 0) --padded.minZ;
    if (padded.maxX == terrain.columns - 1) ++padded.maxX;
    if (padded.maxZ == terrain.rows - 1) ++padded.maxZ;
    gpu.heightUploadRegion.Include(padded);
}

/// @note SampleStitchedTerrainHeight が隣接タイルから読む境界と外周一標本に、編集矩形を写す。
static TerrainHeightRegion NeighborHeightPreview(const TerrainComponent& source, const TerrainComponent& neighbor,
    int dx, int dz, const TerrainHeightRegion& region)
{
    const int offsetX = dx < 0 ? neighbor.columns - 1 : dx > 0 ? 1 - source.columns : 0;
    const int offsetZ = dz < 0 ? neighbor.rows - 1 : dz > 0 ? 1 - source.rows : 0;
    return {
        std::max(region.minX + offsetX, dx < 0 ? neighbor.columns - 1 : -1),
        std::max(region.minZ + offsetZ, dz < 0 ? neighbor.rows - 1 : -1),
        std::min(region.maxX + offsetX, dx > 0 ? 0 : neighbor.columns),
        std::min(region.maxZ + offsetZ, dz > 0 ? 0 : neighbor.rows),
    };
}

static void InvalidateNeighborTerrainChunks(TerrainRenderCache& cache, Scene& scene, renderer::ResourceManager& resources,
    const TerrainGridLookup& gridLookup, EntityID eid, const TerrainComponent& terrain, const TerrainHeightRegion* region)
{
    const auto* position = gridLookup.Find(eid);
    if (!position) return;
    const auto [gx, gz] = *position;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dz == 0) continue;
            const EntityID neighborId = gridLookup.grid->GetCell(gx + dx, gz + dz);
            const auto cached = cache.entities.find(neighborId.index);
            if (cached == cache.entities.end() || cached->second.entityId != neighborId) continue;
            const auto* neighbor = scene.GetComponent<TerrainComponent>(neighborId);
            if (!neighbor) continue;
            if (!region) {
                EraseTerrainChunkCache(cached->second, resources);
                continue;
            }
            auto& gpu = cached->second;
            const auto affected = NeighborHeightPreview(terrain, *neighbor, dx, dz, *region);
            if (!affected.Empty() && gpu.lodCount != 0)
                InvalidatePreviewChunks(resources, *neighbor, gpu, affected, true);
        }
    }
}

struct TerrainChunkUpload {
    TerrainChunk* chunk;
    size_t firstVertex;
    size_t firstIndex;
};

/// @note Limit staging during a full tile load, where all patch vertices and LOD indices can exceed the GPU upload size of a brush dab.
constexpr size_t TERRAIN_CHUNK_BATCH_BYTES = 8 * 1024 * 1024;

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

static void FillChunkHeights(const TerrainComponent& terrain, const TerrainNeighbors& neighbors,
    int x0, int z0, int x1, int z1, int overlap, TerrainChunkScratch& scratch)
{
    scratch.minX = std::max(x0 - overlap, -1);
    scratch.minZ = std::max(z0 - overlap, -1);
    const int maxX = std::min(x1 + overlap, terrain.columns);
    const int maxZ = std::min(z1 + overlap, terrain.rows);
    scratch.stride = static_cast<size_t>(maxX - scratch.minX + 1);
    scratch.heights.resize(scratch.stride * static_cast<size_t>(maxZ - scratch.minZ + 1));
    for (int z = scratch.minZ; z <= maxZ; ++z)
        FillTerrainHeightRow(terrain, neighbors, scratch.minX, maxX, z,
            scratch.heights.data() + static_cast<size_t>(z - scratch.minZ) * scratch.stride, true);
}

static const TerrainChunk& PrepareTerrainChunk(
    TerrainGpuState& gpu,
    const TerrainComponent& terrain,
    int cx,
    int cz,
    const TerrainNeighbors& neighbors,
    size_t lodCount,
    TerrainChunkScratch& scratch)
{
    const TerrainChunkKey key{static_cast<uint32_t>(cx), static_cast<uint32_t>(cz)};
    auto& chunk = gpu.chunks.try_emplace(key).first->second;
    if (!chunk.verticesDirty && !chunk.topologyDirty) return chunk;
    scratch.uploads.push_back({&chunk, scratch.vertices.size(), scratch.indices.size()});

    const int x0 = cx * terrain.chunkSize, z0 = cz * terrain.chunkSize;
    const int x1 = std::min(x0 + terrain.chunkSize, terrain.columns - 1);
    const int z1 = std::min(z0 + terrain.chunkSize, terrain.rows - 1);
    const auto columns = static_cast<uint32_t>(x1 - x0 + 1), rows = static_cast<uint32_t>(z1 - z0 + 1);
    const size_t vertexCount = static_cast<size_t>(columns) * rows;
    chunk.gpuPatch = {static_cast<uint32_t>(x0), static_cast<uint32_t>(z0), columns, rows,
        static_cast<uint32_t>(terrain.columns), static_cast<uint32_t>(terrain.rows), terrain.cellSize, 0};
    auto occlusion = vertexCount <= UINT16_MAX ? std::make_shared<renderer::TerrainOcclusionGeometry>() : nullptr;
    const int overlap = renderer::TERRAIN_LOD_STEPS[lodCount - 1];
    if (chunk.verticesDirty) {
        /// @note Normals, tangents and occlusion columns read the same stitched samples including the patch halo.
        FillChunkHeights(terrain, neighbors, x0, z0, x1, z1, occlusion ? overlap : 1, scratch);
        if (occlusion) occlusion->heights.reserve(vertexCount);
        const float span = 2.0f * terrain.cellSize;
        float minHeight = 1e30f, maxHeight = -1e30f;
        for (int z = z0; z <= z1; ++z) {
            const float* heights = scratch.HeightRow(z);
            const float* north = scratch.HeightRow(z - 1);
            const float* south = scratch.HeightRow(z + 1);
            for (int x = x0; x <= x1; ++x) {
                const int i = x - scratch.minX;
                const float height = heights[i];
                const float dhdx = (heights[i + 1] - heights[i - 1]) / span;
                const float dhdz = (south[i] - north[i]) / span;
                TerrainVertex vertex;
                vertex.position = {static_cast<float>(x) * terrain.cellSize, height, static_cast<float>(z) * terrain.cellSize};
                vertex.normal = math::Vector3{-dhdx, 1.0f, -dhdz}.Normalized();
                vertex.tangent = math::Vector3{terrain.cellSize, dhdx * terrain.cellSize, 0.0f}.Normalized();
                vertex.uv = {static_cast<float>(x) / static_cast<float>(terrain.columns - 1),
                    static_cast<float>(z) / static_cast<float>(terrain.rows - 1)};
                scratch.vertices.push_back(vertex);
                minHeight = std::min(minHeight, height);
                maxHeight = std::max(maxHeight, height);
                if (occlusion) occlusion->heights.push_back(height);
            }
        }
        const float firstX = static_cast<float>(x0) * terrain.cellSize, lastX = static_cast<float>(x1) * terrain.cellSize;
        const float firstZ = static_cast<float>(z0) * terrain.cellSize, lastZ = static_cast<float>(z1) * terrain.cellSize;
        chunk.aabbMin = {std::min({1e30f, firstX, lastX}), minHeight, std::min({1e30f, firstZ, lastZ})};
        chunk.aabbMax = {std::max({-1e30f, firstX, lastX}), maxHeight, std::max({-1e30f, firstZ, lastZ})};
    } else if (occlusion) {
        occlusion->heights = chunk.occlusion->heights;
    }
    if (chunk.topologyDirty) {
        for (size_t lod = 0; lod < lodCount; ++lod) {
            const size_t firstIndex = scratch.indices.size();
            BuildTerrainPatchLodIndices(terrain, x0, z0, x1, z1, renderer::TERRAIN_LOD_STEPS[lod], scratch.indices);
            chunk.indexCountLOD[lod] = static_cast<uint32_t>(scratch.indices.size() - firstIndex);
            if (occlusion) {
                auto& cpuIndices = occlusion->indices[lod];
                cpuIndices.reserve(chunk.indexCountLOD[lod]);
                for (size_t i = firstIndex; i < scratch.indices.size(); ++i)
                    cpuIndices.push_back(static_cast<uint16_t>(scratch.indices[i]));
            }
        }
    } else if (occlusion) {
        /// @note Published views may still read the previous snapshot. Keep its height and topology arrays immutable.
        occlusion->indices = chunk.occlusion->indices;
    }
    if (occlusion && chunk.indexCountLOD[0] == uint32_t((x1 - x0) * (z1 - z0) * 6)) {
        if (!chunk.verticesDirty && !chunk.occlusion->columns.empty()) {
            occlusion->columns = chunk.occlusion->columns;
        } else {
            if (!chunk.verticesDirty) FillChunkHeights(terrain, neighbors, x0, z0, x1, z1, overlap, scratch);
            constexpr int columnStep = 16;
            /// @note 列の境界を最大 LOD の格子に揃え、内側へ張り出す三角形の頂点を高さ下限に含める。
            for (int z = z0; z < z1; z += columnStep) {
                for (int x = x0; x < x1; x += columnStep) {
                    const int lowX = std::max(x - overlap, 0), lowZ = std::max(z - overlap, 0);
                    const int highX = std::min(x + columnStep + overlap, terrain.columns - 1);
                    const int highZ = std::min(z + columnStep + overlap, terrain.rows - 1);
                    float minimum = std::numeric_limits<float>::infinity();
                    for (int sz = lowZ; sz <= highZ; ++sz) {
                        const float* row = scratch.HeightRow(sz);
                        for (int sx = lowX; sx <= highX; ++sx)
                            minimum = std::min(minimum, row[sx - scratch.minX]);
                    }
                    occlusion->columns.push_back({{float(lowX) * terrain.cellSize, float(lowZ) * terrain.cellSize,
                        float(highX) * terrain.cellSize, float(highZ) * terrain.cellSize}, minimum});
                }
            }
        }
    }
    chunk.occlusion = std::move(occlusion);
    return chunk;
}

/// @note Gather CPU geometry before waiting on the renderer. Release and replacement share one ordered resource operation.
static void UploadTerrainChunks(TerrainChunkScratch& scratch, size_t lodCount,
    renderer::IRenderer& renderer, renderer::ResourceManager& resources)
{
    if (scratch.uploads.empty()) return;
    renderer.ExecuteResourceOperation([&] {
        for (const auto& upload : scratch.uploads) {
            auto& chunk = *upload.chunk;
            if (chunk.verticesDirty) {
                const size_t vertexCount = static_cast<size_t>(chunk.gpuPatch.columns) * chunk.gpuPatch.rows;
                resources.Release(chunk.vertexBuffer);
                chunk.vertexBuffer = resources.CreateVertexBuffer(scratch.vertices.data() + upload.firstVertex,
                    vertexCount * sizeof(TerrainVertex), static_cast<uint32_t>(sizeof(TerrainVertex)));
                chunk.verticesDirty = !chunk.vertexBuffer.IsValid();
            }
            if (!chunk.topologyDirty) continue;
            chunk.topologyDirty = false;
            size_t firstIndex = upload.firstIndex;
            for (size_t lod = 0; lod < lodCount; ++lod) {
                auto& buffer = chunk.indexBufferLOD[lod];
                resources.Release(buffer);
                buffer = {};
                /// @note 全セルが穴のチャンクはインデックス 0 本。空バッファは作らず、描画側で飛ばす。
                const uint32_t indexCount = chunk.indexCountLOD[lod];
                if (indexCount == 0) continue;
                buffer = resources.CreateIndexBuffer(scratch.indices.data() + firstIndex, indexCount);
                chunk.topologyDirty |= !buffer.IsValid();
                firstIndex += indexCount;
            }
        }
    });
    scratch.vertices.clear();
    scratch.indices.clear();
    scratch.uploads.clear();
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
    state.splatDynamic = false;
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

static bool UploadSplatRectangle(renderer::ResourceManager& resources,
    renderer::ResourceHandle<renderer::TextureTag> indices, renderer::ResourceHandle<renderer::TextureTag> weights,
    const TerrainComponent& terrain, const TerrainHeightRegion& region)
{
    const auto x = static_cast<uint32_t>(region.minX), z = static_cast<uint32_t>(region.minZ);
    const auto width = static_cast<uint32_t>(region.maxX - region.minX + 1);
    const auto height = static_cast<uint32_t>(region.maxZ - region.minZ + 1);
    const auto rowPitch = static_cast<uint32_t>(terrain.columns) * TERRAIN_SPLAT_SLOTS;
    const auto offset = static_cast<size_t>(z) * rowPitch + static_cast<size_t>(x) * TERRAIN_SPLAT_SLOTS;
    auto* indexTexture = resources.Get(indices);
    auto* weightTexture = resources.Get(weights);
    const auto matches = [&](const renderer::ITexture* texture) {
        return texture && texture->GetWidth() == static_cast<uint32_t>(terrain.columns)
            && texture->GetHeight() == static_cast<uint32_t>(terrain.rows);
    };
    return matches(indexTexture) && matches(weightTexture)
        && indexTexture->UpdateRegion(x, z, width, height, terrain.splatIndices.data() + offset, rowPitch)
        && weightTexture->UpdateRegion(x, z, width, height, terrain.splatWeights.data() + offset, rowPitch);
}

static void UpdateSplatTextures(TerrainRenderCache& cache, TerrainGpuState& gpu, TerrainComponent& terrain,
    renderer::IRenderer& renderer, renderer::ResourceManager& resources)
{
    auto& preview = terrain.splatPreview;
    if (terrain.splatDirty) {
        ++terrain.vegetationRevision;
        terrain.splatDirty = false;
        preview = {};
    } else if (preview.pending) {
        preview.pending = false;
        preview.revision = ++terrain.vegetationRevision;
    }
    if (gpu.vegetationRevision == terrain.vegetationRevision && gpu.splatIndices.IsValid() && gpu.splatWeights.IsValid())
        return;

    FBZZ_PROFILE_SCOPE("Terrain::SplatUpload");
    const bool hasSplat = terrain.HasValidSplat();
    const auto& dirty = preview.region;
    const bool partial = !dirty.Empty() && preview.revision == terrain.vegetationRevision
        && dirty.minX >= 0 && dirty.minZ >= 0 && dirty.maxX < terrain.columns && dirty.maxZ < terrain.rows;
    const TerrainHeightRegion full{0, 0, terrain.columns - 1, terrain.rows - 1};
    const bool canPromote = hasSplat && partial && !gpu.splatDynamic && !cache.splatDynamicUnavailable;
    /// @note Submit both maps in one ordered operation so previous views never read a mixed pair.
    renderer.ExecuteResourceOperation([&] {
        if (hasSplat && gpu.splatDynamic) {
            const auto& region = partial && gpu.vegetationRevision == preview.baseRevision ? dirty : full;
            if (UploadSplatRectangle(resources, gpu.splatIndices, gpu.splatWeights, terrain, region)) return;
        }
        /// @note The first local edit promotes a tile. Idle tiles keep their original immutable textures.
        if (canPromote) {
            const auto width = static_cast<uint32_t>(terrain.columns);
            const auto height = static_cast<uint32_t>(terrain.rows);
            const auto indices = resources.CreateDynamicTexture(width, height, renderer::DynamicTextureFormat::RGBA8);
            const auto weights = indices.IsValid()
                ? resources.CreateDynamicTexture(width, height, renderer::DynamicTextureFormat::RGBA8)
                : renderer::ResourceHandle<renderer::TextureTag>{};
            if (UploadSplatRectangle(resources, indices, weights, terrain, full)) {
                resources.Release(gpu.splatIndices);
                resources.Release(gpu.splatWeights);
                gpu.splatIndices = indices;
                gpu.splatWeights = weights;
                gpu.splatDynamic = true;
                return;
            }
            resources.Release(indices);
            resources.Release(weights);
            if (!indices.IsValid()) cache.splatDynamicUnavailable = true;
        }
        RebuildSplatTextures(gpu, terrain, resources);
    });
    gpu.vegetationRevision = terrain.vegetationRevision;
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

static void UpdateTerrainGpuHeights(TerrainRenderCache& cache, TerrainGpuState& state, const TerrainComponent& terrain,
    const TerrainNeighbors& neighbors, renderer::IRenderer& renderer, renderer::ResourceManager& resources)
{
    if (!state.heightsDirty && state.heightUploadRegion.Empty() && resources.Get(state.heights)) return;
    FBZZ_PROFILE_SCOPE("Terrain::HeightUpload");
    const size_t stride = static_cast<size_t>(terrain.columns) + 2;
    const size_t count = stride * (static_cast<size_t>(terrain.rows) + 2);
    const auto* current = resources.Get(state.heights);
    const bool sameShape = current && current->GetElementCount() == count;
    const auto& region = state.heightUploadRegion;
    const bool local = !state.heightsDirty && !region.Empty() && sameShape;
    if (local && state.heightsDynamic) {
        const size_t width = static_cast<size_t>(region.maxX - region.minX + 1);
        const size_t rows = static_cast<size_t>(region.maxZ - region.minZ + 1);
        std::vector<float> heights(width * rows);
        for (int z = region.minZ; z <= region.maxZ; ++z)
            FillTerrainHeightRow(terrain, neighbors, region.minX, region.maxX, z,
                heights.data() + static_cast<size_t>(z - region.minZ) * width);
        const size_t offset = static_cast<size_t>(region.minZ + 1) * stride + region.minX + 1;
        bool uploaded = false;
        /// @note The default-heap buffer copies stay ordered with prior GPU draws. Batch CPU executor synchronization.
        renderer.ExecuteResourceOperation([&] {
            if (auto* buffer = resources.Get(state.heights))
                uploaded = buffer->UpdateRows(heights.data(), width * sizeof(float), rows,
                    stride * sizeof(float), offset * sizeof(float));
        });
        if (uploaded) {
            state.heightUploadRegion = {};
            return;
        }
    }

    std::vector<float> heights(count);
    for (int z = -1; z <= terrain.rows; ++z)
        FillTerrainHeightRow(terrain, neighbors, -1, terrain.columns, z,
            heights.data() + static_cast<size_t>(z + 1) * stride);
    bool reused = false;
    if (state.heightsDynamic && sameShape)
        reused = resources.UpdateRegion(state.heights, heights.data(), heights.size() * sizeof(float), 0);
    if (!reused) {
        resources.Release(state.heights);
        state.heights = {};
        state.heightsDynamic = false;
        if (local && !cache.heightDynamicUnavailable) {
            renderer.ExecuteResourceOperation([&] {
                const auto writable = resources.CreateRWStructuredBuffer(nullptr, static_cast<uint32_t>(count), sizeof(float));
                if (resources.UpdateRegion(writable, heights.data(), heights.size() * sizeof(float), 0)) {
                    state.heights = writable;
                    state.heightsDynamic = true;
                } else {
                    resources.Release(writable);
                }
            });
            if (!state.heightsDynamic) cache.heightDynamicUnavailable = true;
        }
        /// @note Replace the immutable SRV if ordered updates are unavailable, including after a partly staged rectangle.
        if (!state.heightsDynamic)
            state.heights = resources.CreateStructuredBuffer(heights.data(), static_cast<uint32_t>(count), sizeof(float));
    }
    state.heightsDirty = !resources.Get(state.heights);
    state.heightUploadRegion = {};
}

static void PrepareTerrainGpuGeometry(TerrainGpuState& state, const TerrainComponent& terrain,
    const TerrainNeighbors& neighbors, renderer::RenderTerrainInput& input,
    TerrainRenderCache& cache, renderer::IRenderer& renderer, renderer::ResourceManager& resources)
{
    if (input.patches.empty()) return;
    if (state.geometryDirty) {
        resources.Release(state.patches);
        resources.Release(state.indices);
        state.patches = {}; state.indices = {};
        state.indexStarts.clear();
        state.indexCount = 0; state.vertexCapacity = 0;
        std::vector<renderer::TerrainGpuPatch> patches;
        std::vector<uint32_t> indices;
        patches.reserve(input.patches.size());
        size_t indexCount = 0;
        for (const auto& chunk : input.patches)
            for (size_t lod = 0; lod < input.LODCount(); ++lod) indexCount += chunk.indexCountLOD[lod];
        indices.reserve(indexCount);
        for (const auto& chunk : input.patches) {
            const auto& patch = chunk.gpuPatch;
            patches.push_back(patch);
            decltype(chunk.gpuStartIndexLOD) starts{};
            for (size_t lod = 0; lod < input.LODCount(); ++lod) {
                starts[lod] = static_cast<uint32_t>(indices.size());
                if (chunk.occlusion) {
                    const auto& cpuIndices = chunk.occlusion->indices[lod];
                    indices.insert(indices.end(), cpuIndices.begin(), cpuIndices.end());
                } else {
                    BuildTerrainPatchLodIndices(terrain, static_cast<int>(patch.originX), static_cast<int>(patch.originZ),
                        static_cast<int>(patch.originX + patch.columns - 1),
                        static_cast<int>(patch.originZ + patch.rows - 1), renderer::TERRAIN_LOD_STEPS[lod], indices);
                }
            }
            state.indexStarts.push_back(starts);
            state.vertexCapacity = std::max(state.vertexCapacity, patch.columns * patch.rows);
        }
        state.patches = resources.CreateStructuredBuffer(patches.data(), static_cast<uint32_t>(patches.size()), sizeof(renderer::TerrainGpuPatch));
        state.indexCount = static_cast<uint32_t>(indices.size());
        state.indices = resources.CreateIndexBuffer(indices.data(), state.indexCount);
        state.geometryDirty = !resources.Get(state.patches) || !resources.Get(state.indices);
    }
    UpdateTerrainGpuHeights(cache, state, terrain, neighbors, renderer, resources);
    if (state.geometryDirty || state.heightsDirty || state.indexStarts.size() != input.patches.size()) return;
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
static TerrainChunk& EnsureSurfaceChunk(TerrainGpuState& gpu, const TerrainComponent& terrain, size_t first,
    renderer::ResourceManager& resources)
{
    const TerrainChunkKey key{UINT32_MAX, static_cast<uint32_t>(first / TERRAIN_SURFACE_PATCH_INDICES)};
    const auto found = gpu.chunks.find(key);
    if (found != gpu.chunks.end()) return found->second;
    const auto data = BuildTerrainSurfacePatch(terrain, first);
    TerrainChunk chunk;
    chunk.aabbMin = data.aabbMin; chunk.aabbMax = data.aabbMax;
    chunk.vertexBuffer = resources.CreateVertexBuffer(data.vertices.data(), data.vertices.size() * sizeof(TerrainVertex), sizeof(TerrainVertex));
    for (size_t lod = 0; lod < renderer::TERRAIN_FAR_LOD_INDEX; ++lod) {
        chunk.indexCountLOD[lod] = static_cast<uint32_t>(data.indices.size());
        chunk.indexBufferLOD[lod] = resources.CreateIndexBuffer(data.indices.data(), chunk.indexCountLOD[lod]);
    }
    return gpu.chunks.emplace(key, std::move(chunk)).first->second;
}
static void ExtractTerrainSurface(TerrainRenderCache& cache, const TerrainComponent& terrain, TerrainGpuState& gpu,
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
        surface.patches.push_back(EnsureSurfaceChunk(gpu, terrain, first, resources));
    /// @note No GPU heightfield decoder and no terrain grass on cave ceilings; camera/shadow/mask consume these same patches.
    output.terrains.push_back(std::move(surface));
}

math::Vector4 GetTerrainGrassWaterClip(Scene& scene)
{
    math::Vector4 terrainWaterClip{};
    for (EntityID id : scene.GetEntities<WaterComponent>()) {
        const auto* owner = scene.GetGameObject(id);
        const auto* water = scene.GetComponent<WaterComponent>(id);
        if (!owner || !owner->activeInHierarchy() || !water || !water->enabled
            || !WaterSurfaceUsesHorizon(*water) || !IsWaterSurfaceSupported(*water, owner->transform)) continue;
        const auto* exclusions = WaterSurfaceExclusions(*water, WaterSurfaceReadView::LIVE_QUERY);
        if (exclusions && (!exclusions->valid || !exclusions->boxes.empty())) continue;
        /// @note Match the PCG shoreline buffer. The still waterline keeps vegetation fixed while waves move.
        const float dryHeight = owner->transform.worldPosition.y + 0.15f;
        if (terrainWaterClip.y == 0.0f || dryHeight > terrainWaterClip.x)
            terrainWaterClip = {dryHeight, 1.0f, 0.0f, 0.0f};
    }
    return terrainWaterClip;
}

void ExtractRenderTerrains(RenderPassContext& ctx, renderer::RenderScene& output) {
    auto& scene = ctx.scene;
    auto& resources = ctx.resources;
    auto& cache = AcquireTerrainRenderCache(scene, resources);
    {
        std::unordered_set<uint32_t> validIndices;
        for (EntityID eid : scene.GetEntities<TerrainComponent>())
            validIndices.insert(eid.index);

        std::erase_if(cache.entities, [&scene, &validIndices, &resources](auto& kv) {
            if (validIndices.contains(kv.first) && scene.IsValid(kv.second.entityId)) return false;
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
    const TerrainGridLookup gridLookup(terrainGrid);

    /// @note 天候はシーンに 1 つ。TerrainGrid では地形が何十個も回るのでループの外で引く。
    const ActiveWeather weather = FindActiveWeather(scene);
    const auto drawMode = ctx.settings.geometryDrawMode;
    const bool useGpuGeometry = ctx.renderer.SupportsIndirectDraw()
        && (drawMode == renderer::GeometryDrawMode::GPU_INDIRECT || drawMode == renderer::GeometryDrawMode::MESH_SHADER);

    const TerrainCandidateLease candidates(scene);
    const RenderSelectionLookup selection(ctx.settings.selectedObjects);
    bool collectPhases = false;
    if (profiler::Profiler::IsEnabled()) {
        const auto filter = profiler::Profiler::GetRequestedCaptureFilter();
        collectPhases = filter.scope == "RenderSceneExtractor::Terrains"
            && (filter.category.empty() || filter.category == "General");
    }
    std::optional<math::Vector4> grassWaterClip;
    TerrainChunkScratch scratch;
    int64_t layersNs = 0, patchesNs = 0, setupNs = 0;
    /// @note 隣接タイルを先に失効させ、同じフレームの出力が破棄済みバッファを参照しないようにする。
    for (const EntityID eid : candidates.Entities()) {
        TerrainPhaseTimer setupTimer(collectPhases, setupNs);
        const auto* owner = scene.GetGameObject(eid);
        auto* terrain = scene.GetComponent<TerrainComponent>(eid);
        if (!owner || !terrain) continue;
        /// @note Cached visible tiles can sample authored heights from inactive or offscreen neighbors.
        const bool drawn = terrain->enabled && !terrain->heightData.empty() && owner->activeInHierarchy();
        if (!drawn && !terrainGrid) continue;
        auto& gpu = cache.entities[eid.index];
        if (gpu.entityId != eid) {
            ReleaseTerrainGpuState(gpu, resources);
            gpu.entityId = eid;
        }
        if (terrain->heightDirty) {
            ++terrain->surfaceRevision;
            terrain->heightDirty = false;
        }
        auto& preview = terrain->heightPreview;
        preview.pending = false;
        if (gpu.surfaceRevision == terrain->surfaceRevision) continue;

        const bool partial = gpu.lodCount != 0 && preview.revision == terrain->surfaceRevision
            && preview.baseRevision == gpu.surfaceRevision;
        if (partial) InvalidatePreviewChunks(resources, *terrain, gpu, preview.region,
            preview.heightChanged, preview.topologyChanged);
        else EraseTerrainChunkCache(gpu, resources);
        if (!partial || preview.heightChanged)
            InvalidateNeighborTerrainChunks(cache, scene, resources, gridLookup, eid, *terrain,
                partial ? &preview.region : nullptr);
        gpu.surfaceRevision = terrain->surfaceRevision;
    }
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
        const TerrainNeighbors neighbors = ResolveTerrainNeighbors(scene, gridLookup, eid);

        TerrainGpuState& gpu = cache.entities[eid.index];
        if (gpu.lodCount != lodCount) {
            EraseTerrainChunkCache(gpu, resources);
            gpu.lodCount = lodCount;
        }
        UpdateSplatTextures(cache, gpu, terrain, ctx.renderer, resources);
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
        terrainCBData.weather = { weather.wetness, weather.darkening, weather.puddleAmount,
            std::isfinite(terrain.farLodDistance) ? (std::max)(terrain.farLodDistance, 0.0f) : 0.0f };
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
                if (!grassWaterClip) grassWaterClip = GetTerrainGrassWaterClip(scene);
                input.constants.grassGroundWaterClip = *grassWaterClip;
                input.constants.grassGroundLod = {nearDistance, fullDistance, static_cast<float>(fiber->m_terrainLayer), threshold};
                input.constants.grassGroundColor = {std::clamp(color.x, 0.0f, 1.0f), std::clamp(color.y, 0.0f, 1.0f),
                    std::clamp(color.z, 0.0f, 1.0f), std::clamp(fiber->m_terrainGroundCover, 0.0f, 1.0f)};
            }
        }
        input.selected = selection.Contains(eid);
        const bool useRasterStrip = ctx.settings.rasterGeometryMode == renderer::RasterGeometryMode::TRIANGLE_STRIP;
        /// @note Inserting chunks can rehash the cache, but references to its elements stay valid.
        scratch.drawChunks.clear();
        for (int cz = 0; cz < chunkCountZ; ++cz) {
            for (int cx = 0; cx < chunkCountX; ++cx) {
                const auto& chunk = PrepareTerrainChunk(gpu, terrain, cx, cz, neighbors, lodCount, scratch);
                if (chunk.indexCountLOD[0] != 0) scratch.drawChunks.push_back(&chunk);
                if (scratch.vertices.size() * sizeof(TerrainVertex) + scratch.indices.size() * sizeof(uint32_t)
                    >= TERRAIN_CHUNK_BATCH_BYTES) UploadTerrainChunks(scratch, lodCount, ctx.renderer, resources);
            }
        }
        UploadTerrainChunks(scratch, lodCount, ctx.renderer, resources);
        input.patches.reserve(scratch.drawChunks.size());
        for (const auto* chunk : scratch.drawChunks) {
            auto& patch = input.patches.emplace_back(*chunk);
            if (useRasterStrip) patch.lod0Strip = EnsureTerrainStripIndices(gpu, *chunk,
                static_cast<uint32_t>(terrain.chunkSize) + 1u, resources);
        }
        if (useGpuGeometry) PrepareTerrainGpuGeometry(gpu, terrain, neighbors, input, cache, ctx.renderer, resources);
        if ((ctx.directionalShadowBakeEnabled || ctx.staticShadowCache.cascadeMask != 0) && !input.patches.empty()) {
            if (!gpu.shadowBakeGeometryHash)
                gpu.shadowBakeGeometryHash = TerrainShadowBakeGeometryHash(terrain, input.patches);
            input.shadowBakeGeometryHash = gpu.shadowBakeGeometryHash;
            input.shadowBakeStatic = input.shadowBakeGeometryHash != 0;
        }
        ExtractTerrainSurface(cache, terrain, gpu, input, cache.fallback, resources, output);
        output.terrains.push_back(std::move(input));
    }
    if (collectPhases) {
        profiler::Profiler::PushSample({"RenderSceneExtractor::Terrains::Setup", "General"}, static_cast<double>(setupNs) / 1000000.0);
        profiler::Profiler::PushSample({"RenderSceneExtractor::Terrains::Layers", "General"}, static_cast<double>(layersNs) / 1000000.0);
        profiler::Profiler::PushSample({"RenderSceneExtractor::Terrains::Patches", "General"}, static_cast<double>(patchesNs) / 1000000.0);
    }
}
}
