/// @file    RenderTerrainGrid.cpp
/// @brief   隣接地形の高さ接合とジオメトリ変更の反映。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "RenderTerrainExtraction.hpp"
#include "TerrainHeightHelpers.hpp"
#include <Engine/Scene/Components/TerrainGridComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <algorithm>
#include <unordered_set>

namespace fbzz::scene::terrain_extraction_detail {

namespace {

using terrain_height_detail::TryAddTerrainHeightSample;

uint64_t TerrainEntityKey(EntityID entity) { return (uint64_t(entity.generation) << 32) | entity.index; }

static float SampleStitchedTerrainHeight(
    const TerrainComponent& terrain,
    const TerrainNeighbors& neighbors,
    int                     columnX,
    int                     rowZ)
{
    /// @note TerrainGrid の境界では複数 Terrain が同じワールド頂点を持つ。描画用メッシュでは
    /// @note その共有頂点を平均し、保存データを破壊せずに隙間を隠す。角は最大 4 Terrain が
    /// @note 交差するため、斜め隣接も平均に含める。
    float heightSum = 0.0f;
    int sampleCount = 0;

    TryAddTerrainHeightSample(&terrain, columnX, rowZ, heightSum, sampleCount);

    if (columnX <= 0) {
        TryAddTerrainHeightSample(neighbors.west, neighbors.west ? neighbors.west->columns - 1 + columnX : columnX, rowZ, heightSum, sampleCount);
    }
    if (columnX >= terrain.columns - 1) {
        TryAddTerrainHeightSample(neighbors.east, columnX - (terrain.columns - 1), rowZ, heightSum, sampleCount);
    }
    if (rowZ <= 0) {
        TryAddTerrainHeightSample(neighbors.north, columnX, neighbors.north ? neighbors.north->rows - 1 + rowZ : rowZ, heightSum, sampleCount);
    }
    if (rowZ >= terrain.rows - 1) {
        TryAddTerrainHeightSample(neighbors.south, columnX, rowZ - (terrain.rows - 1), heightSum, sampleCount);
    }

    if (columnX <= 0 && rowZ <= 0) {
        TryAddTerrainHeightSample(
            neighbors.northWest,
            neighbors.northWest ? neighbors.northWest->columns - 1 + columnX : columnX,
            neighbors.northWest ? neighbors.northWest->rows - 1 + rowZ : rowZ,
            heightSum,
            sampleCount);
    }
    if (columnX >= terrain.columns - 1 && rowZ <= 0) {
        TryAddTerrainHeightSample(
            neighbors.northEast,
            columnX - (terrain.columns - 1),
            neighbors.northEast ? neighbors.northEast->rows - 1 + rowZ : rowZ,
            heightSum,
            sampleCount);
    }
    if (columnX <= 0 && rowZ >= terrain.rows - 1) {
        TryAddTerrainHeightSample(
            neighbors.southWest,
            neighbors.southWest ? neighbors.southWest->columns - 1 + columnX : columnX,
            rowZ - (terrain.rows - 1),
            heightSum,
            sampleCount);
    }
    if (columnX >= terrain.columns - 1 && rowZ >= terrain.rows - 1) {
        TryAddTerrainHeightSample(
            neighbors.southEast,
            columnX - (terrain.columns - 1),
            rowZ - (terrain.rows - 1),
            heightSum,
            sampleCount);
    }

    if (sampleCount > 0)
        return heightSum / static_cast<float>(sampleCount);

    const int clampedX = std::clamp(columnX, 0, terrain.columns - 1);
    const int clampedZ = std::clamp(rowZ, 0, terrain.rows - 1);
    const size_t sampleIndex = static_cast<size_t>(clampedZ) * static_cast<size_t>(terrain.columns)
                     + static_cast<size_t>(clampedX);
    return terrain.heightData[sampleIndex] * terrain.maxHeight;
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
            /// @note 穴の矩形は最終セルの奥側の頂点まで含む。各 LOD の穴判定はパッチ内に収める。
            chunk->second.topologyDirty |= topologyChanged
                && x * terrain.chunkSize < region.maxX && (x + 1) * terrain.chunkSize > region.minX
                && z * terrain.chunkSize < region.maxZ && (z + 1) * terrain.chunkSize > region.minZ;
        }
    }
    gpu.geometryDirty |= topologyChanged;
    gpu.shadowBakeGeometryHash = 0;
    if (!heightChanged) return;
    /// @note 隣接タイルがない場合は、高さバッファの余白に自タイル端のサンプルを複製する。
    TerrainHeightRegion paddedRegion = region;
    if (paddedRegion.minX == 0) --paddedRegion.minX;
    if (paddedRegion.minZ == 0) --paddedRegion.minZ;
    if (paddedRegion.maxX == terrain.columns - 1) ++paddedRegion.maxX;
    if (paddedRegion.maxZ == terrain.rows - 1) ++paddedRegion.maxZ;
    gpu.heightUploadRegion.Include(paddedRegion);
}

/// @note SampleStitchedTerrainHeight が隣接タイルから読む境界と外周一標本に、編集矩形を写す。
static TerrainHeightRegion NeighborHeightPreview(const TerrainComponent& source, const TerrainComponent& neighbor,
    int neighborOffsetX, int neighborOffsetZ, const TerrainHeightRegion& region)
{
    const int offsetX = neighborOffsetX < 0 ? neighbor.columns - 1 : neighborOffsetX > 0 ? 1 - source.columns : 0;
    const int offsetZ = neighborOffsetZ < 0 ? neighbor.rows - 1 : neighborOffsetZ > 0 ? 1 - source.rows : 0;
    return {
        std::max(region.minX + offsetX, neighborOffsetX < 0 ? neighbor.columns - 1 : -1),
        std::max(region.minZ + offsetZ, neighborOffsetZ < 0 ? neighbor.rows - 1 : -1),
        std::min(region.maxX + offsetX, neighborOffsetX > 0 ? 0 : neighbor.columns),
        std::min(region.maxZ + offsetZ, neighborOffsetZ > 0 ? 0 : neighbor.rows),
    };
}

static void InvalidateNeighborTerrainChunks(TerrainRenderCache& cache, Scene& scene, renderer::ResourceManager& resources,
    const TerrainGridLookup& gridLookup, EntityID entity, const TerrainComponent& terrain, const TerrainHeightRegion* region)
{
    const auto* position = gridLookup.Find(entity);
    if (!position) return;
    const auto [gridX, gridZ] = *position;
    for (int neighborOffsetZ = -1; neighborOffsetZ <= 1; ++neighborOffsetZ) {
        for (int neighborOffsetX = -1; neighborOffsetX <= 1; ++neighborOffsetX) {
            if (neighborOffsetX == 0 && neighborOffsetZ == 0) continue;
            const EntityID neighborId = gridLookup.Grid()->GetCell(gridX + neighborOffsetX, gridZ + neighborOffsetZ);
            const auto cached = cache.entities.find(neighborId.index);
            if (cached == cache.entities.end() || cached->second.entityId != neighborId) continue;
            const auto* neighbor = scene.GetComponent<TerrainComponent>(neighborId);
            if (!neighbor) continue;
            if (!region) {
                EraseTerrainChunkCache(cached->second, resources);
                continue;
            }
            auto& gpu = cached->second;
            const auto affected = NeighborHeightPreview(terrain, *neighbor, neighborOffsetX, neighborOffsetZ, *region);
            if (!affected.Empty() && gpu.lodCount != 0)
                InvalidatePreviewChunks(resources, *neighbor, gpu, affected, true);
        }
    }
}

}

TerrainGridLookup::TerrainGridLookup(const TerrainGridComponent* terrainGrid)
    : m_grid(terrainGrid)
{
    if (!m_grid || m_grid->cellCountX <= 0 || m_grid->cellCountZ <= 0) return;
    const size_t gridWidth = static_cast<size_t>(m_grid->cellCountX);
    const size_t cellCount = std::min(m_grid->cells.size(), gridWidth * static_cast<size_t>(m_grid->cellCountZ));
    m_positions.reserve(cellCount);
    for (size_t cellIndex = 0; cellIndex < cellCount; ++cellIndex) {
        const auto entity = m_grid->cells[cellIndex];
        /// @note 重複参照は TryGetGridPos と同じく行優先で最初のセルを使う。
        if (entity.IsValid()) m_positions.try_emplace(TerrainEntityKey(entity),
            std::array{static_cast<int>(cellIndex % gridWidth), static_cast<int>(cellIndex / gridWidth)});
    }
}

const std::array<int, 2>* TerrainGridLookup::Find(EntityID entity) const
{
    const auto found = m_positions.find(TerrainEntityKey(entity));
    return found == m_positions.end() ? nullptr : &found->second;
}

const TerrainGridComponent* FindActiveTerrainGrid(Scene& scene)
{
    /// @note 親ごと無効化された Grid は使わない (有効な最初の 1 つ)。
    for (const EntityID gridId : scene.GetEntities<TerrainGridComponent>()) {
        const GameObject* gridObject = scene.GetGameObject(gridId);
        if (!gridObject || !gridObject->activeInHierarchy()) continue;
        return scene.GetComponent<TerrainGridComponent>(gridId);
    }
    return nullptr;
}

TerrainNeighbors ResolveTerrainNeighbors(
    Scene&                       scene,
    const TerrainGridLookup&     gridLookup,
    EntityID                     entity)
{
    TerrainNeighbors neighbors;
    const auto* position = gridLookup.Find(entity);
    if (!position) return neighbors;
    const auto [gridX, gridZ] = *position;

    auto resolveNeighbor = [&](int neighborX, int neighborZ) -> const TerrainComponent* {
        const EntityID neighbor = gridLookup.Grid()->GetCell(neighborX, neighborZ);
        if (!scene.IsValid(neighbor))
            return nullptr;
        return scene.GetComponent<TerrainComponent>(neighbor);
    };

    neighbors.north     = resolveNeighbor(gridX,     gridZ - 1);
    neighbors.south     = resolveNeighbor(gridX,     gridZ + 1);
    neighbors.west      = resolveNeighbor(gridX - 1, gridZ);
    neighbors.east      = resolveNeighbor(gridX + 1, gridZ);
    neighbors.northWest = resolveNeighbor(gridX - 1, gridZ - 1);
    neighbors.northEast = resolveNeighbor(gridX + 1, gridZ - 1);
    neighbors.southWest = resolveNeighbor(gridX - 1, gridZ + 1);
    neighbors.southEast = resolveNeighbor(gridX + 1, gridZ + 1);
    return neighbors;
}

/// @note 隣接タイルとの継ぎ目を含む高さを渡し、GPU の中央差分を CPU 側の計算と揃える。
void FillTerrainHeightRow(const TerrainComponent& terrain, const TerrainNeighbors& neighbors,
    int firstX, int lastX, int rowZ, float* output, bool canonicalZero)
{
    const auto sampleColumn = [&](int columnX) { output[columnX - firstX] = SampleStitchedTerrainHeight(terrain, neighbors, columnX, rowZ); };
    if (rowZ <= 0 || rowZ >= terrain.rows - 1) {
        for (int columnX = firstX; columnX <= lastX; ++columnX) sampleColumn(columnX);
        return;
    }
    for (int columnX = firstX; columnX <= (std::min)(lastX, 0); ++columnX) sampleColumn(columnX);
    const int begin = (std::max)(firstX, 1), end = (std::min)(lastX, terrain.columns - 2);
    if (begin <= end) {
        const auto* source = terrain.heightData.data() + static_cast<size_t>(rowZ) * terrain.columns;
        /// @note CPU パッチのゼロの符号は、継ぎ目での累積計算に揃える。
        if (canonicalZero) {
            std::transform(source + begin, source + end + 1, output + (begin - firstX),
                [maxHeight = terrain.maxHeight](float height) { return 0.0f + height * maxHeight; });
        } else {
            std::transform(source + begin, source + end + 1, output + (begin - firstX),
                [maxHeight = terrain.maxHeight](float height) { return height * maxHeight; });
        }
    }
    for (int columnX = (std::max)(firstX, terrain.columns - 1); columnX <= lastX; ++columnX) sampleColumn(columnX);
}

void EraseTerrainChunkCache(TerrainGpuState& gpu, renderer::ResourceManager& resources)
{
    gpu.geometryDirty = true;
    gpu.heightsDirty = true;
    gpu.heightUploadRegion = {};
    gpu.shadowBakeGeometryHash = 0;
    ReleaseTerrainChunks(gpu, resources);
}

void ReleaseRemovedTerrainResources(TerrainRenderCache& cache, Scene& scene, renderer::ResourceManager& resources)
{
    std::unordered_set<uint32_t> validIndices;
    for (EntityID eid : scene.GetEntities<TerrainComponent>())
        validIndices.insert(eid.index);

    std::erase_if(cache.entities, [&scene, &validIndices, &resources](auto& entry) {
        if (validIndices.contains(entry.first) && scene.IsValid(entry.second.entityId)) return false;
        ReleaseTerrainGpuState(entry.second, resources);
        return true;
    });
}

void RefreshTerrainGeometry(TerrainRenderCache& cache, Scene& scene, const TerrainGridLookup& gridLookup,
    EntityID entity, renderer::ResourceManager& resources)
{
    const auto* owner = scene.GetGameObject(entity);
    auto* terrain = scene.GetComponent<TerrainComponent>(entity);
    if (!owner || !terrain) return;
    /// @note 可視タイルのキャッシュも、非アクティブまたは画面外の隣接タイルに設定された高さを参照できる。
    const bool isDrawable = terrain->enabled && !terrain->heightData.empty() && owner->activeInHierarchy();
    if (!isDrawable && !gridLookup.Grid()) return;
    auto& gpu = cache.entities[entity.index];
    if (gpu.entityId != entity) {
        ReleaseTerrainGpuState(gpu, resources);
        gpu.entityId = entity;
    }
    if (terrain->heightDirty) {
        ++terrain->surfaceRevision;
        terrain->heightDirty = false;
    }
    auto& preview = terrain->heightPreview;
    preview.pending = false;
    if (gpu.surfaceRevision == terrain->surfaceRevision) return;

    const bool hasPartialUpdate = gpu.lodCount != 0 && preview.revision == terrain->surfaceRevision
        && preview.baseRevision == gpu.surfaceRevision;
    if (hasPartialUpdate) InvalidatePreviewChunks(resources, *terrain, gpu, preview.region,
        preview.heightChanged, preview.topologyChanged);
    else EraseTerrainChunkCache(gpu, resources);
    if (!hasPartialUpdate || preview.heightChanged)
        InvalidateNeighborTerrainChunks(cache, scene, resources, gridLookup, entity, *terrain,
            hasPartialUpdate ? &preview.region : nullptr);
    gpu.surfaceRevision = terrain->surfaceRevision;
}

}
