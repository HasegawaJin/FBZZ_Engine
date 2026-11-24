/// @file    RenderTerrainMesh.cpp
/// @brief   地形パッチのメッシュ準備・アップロードとサーフェスの発行。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "RenderTerrainExtraction.hpp"
#include <Engine/Rendering/Passes/RenderPassContext.hpp>
#include "../../Passes/Geometry/TerrainPatchIndices.hpp"
#include <Engine/Asset/Flipbook/BakeFingerprint.hpp>
#include <Engine/Scene/Terrain/TerrainSurfaceGeometry.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/Resources/ResourceManager.hpp>
#include <Graphics/Renderer/Geometry/SurfacePatchLod.hpp>
#include <algorithm>
#include <charconv>
#include <limits>
#include <span>
#include <utility>

namespace fbzz::scene::terrain_extraction_detail {

namespace {

/// @note タイル全体の頂点と LOD インデックスはブラシの局所更新より大きいため、一度に転送準備する量を制限する。
constexpr size_t TERRAIN_CHUNK_BATCH_BYTES = 8 * 1024 * 1024;

static void FillChunkHeights(const TerrainComponent& terrain, const TerrainNeighbors& neighbors,
    int firstColumn, int firstRow, int lastColumn, int lastRow, int overlap, TerrainChunkScratch& scratch)
{
    scratch.minX = std::max(firstColumn - overlap, -1);
    scratch.minZ = std::max(firstRow - overlap, -1);
    const int maxX = std::min(lastColumn + overlap, terrain.columns);
    const int maxZ = std::min(lastRow + overlap, terrain.rows);
    scratch.stride = static_cast<size_t>(maxX - scratch.minX + 1);
    scratch.heights.resize(scratch.stride * static_cast<size_t>(maxZ - scratch.minZ + 1));
    for (int z = scratch.minZ; z <= maxZ; ++z)
        FillTerrainHeightRow(terrain, neighbors, scratch.minX, maxX, z,
            scratch.heights.data() + static_cast<size_t>(z - scratch.minZ) * scratch.stride, true);
}

static const TerrainChunk& PrepareTerrainChunk(
    TerrainGpuState& gpu,
    const TerrainComponent& terrain,
    int chunkX,
    int chunkZ,
    const TerrainNeighbors& neighbors,
    size_t lodCount,
    TerrainChunkScratch& scratch)
{
    const TerrainChunkKey key{static_cast<uint32_t>(chunkX), static_cast<uint32_t>(chunkZ)};
    auto& chunk = gpu.chunks.try_emplace(key).first->second;
    if (!chunk.verticesDirty && !chunk.topologyDirty) return chunk;
    scratch.uploads.push_back({&chunk, scratch.vertices.size(), scratch.indices.size()});

    const int firstColumn = chunkX * terrain.chunkSize, firstRow = chunkZ * terrain.chunkSize;
    const int lastColumn = std::min(firstColumn + terrain.chunkSize, terrain.columns - 1);
    const int lastRow = std::min(firstRow + terrain.chunkSize, terrain.rows - 1);
    const auto columns = static_cast<uint32_t>(lastColumn - firstColumn + 1), rows = static_cast<uint32_t>(lastRow - firstRow + 1);
    const size_t vertexCount = static_cast<size_t>(columns) * rows;
    chunk.gpuPatch = {static_cast<uint32_t>(firstColumn), static_cast<uint32_t>(firstRow), columns, rows,
        static_cast<uint32_t>(terrain.columns), static_cast<uint32_t>(terrain.rows), terrain.cellSize, 0};
    auto occlusion = vertexCount <= UINT16_MAX ? std::make_shared<renderer::TerrainOcclusionGeometry>() : nullptr;
    const int overlap = renderer::TERRAIN_LOD_STEPS[lodCount - 1];
    if (chunk.verticesDirty) {
        /// @note 法線、接線、遮蔽用の列には、パッチ外周の余白を含めて同じ縫合済みサンプルを使う。
        FillChunkHeights(terrain, neighbors, firstColumn, firstRow, lastColumn, lastRow, occlusion ? overlap : 1, scratch);
        if (occlusion) occlusion->heights.reserve(vertexCount);
        const float sampleDistance = 2.0f * terrain.cellSize;
        float minHeight = 1e30f, maxHeight = -1e30f;
        for (int z = firstRow; z <= lastRow; ++z) {
            const float* heights = scratch.HeightRow(z);
            const float* north = scratch.HeightRow(z - 1);
            const float* south = scratch.HeightRow(z + 1);
            for (int x = firstColumn; x <= lastColumn; ++x) {
                const int i = x - scratch.minX;
                const float height = heights[i];
                const float heightSlopeX = (heights[i + 1] - heights[i - 1]) / sampleDistance;
                const float heightSlopeZ = (south[i] - north[i]) / sampleDistance;
                TerrainVertex vertex;
                vertex.position = {static_cast<float>(x) * terrain.cellSize, height, static_cast<float>(z) * terrain.cellSize};
                vertex.normal = math::Vector3{-heightSlopeX, 1.0f, -heightSlopeZ}.Normalized();
                vertex.tangent = math::Vector3{terrain.cellSize, heightSlopeX * terrain.cellSize, 0.0f}.Normalized();
                vertex.uv = {static_cast<float>(x) / static_cast<float>(terrain.columns - 1),
                    static_cast<float>(z) / static_cast<float>(terrain.rows - 1)};
                scratch.vertices.push_back(vertex);
                minHeight = std::min(minHeight, height);
                maxHeight = std::max(maxHeight, height);
                if (occlusion) occlusion->heights.push_back(height);
            }
        }
        const float firstX = static_cast<float>(firstColumn) * terrain.cellSize, lastX = static_cast<float>(lastColumn) * terrain.cellSize;
        const float firstZ = static_cast<float>(firstRow) * terrain.cellSize, lastZ = static_cast<float>(lastRow) * terrain.cellSize;
        chunk.aabbMin = {std::min({1e30f, firstX, lastX}), minHeight, std::min({1e30f, firstZ, lastZ})};
        chunk.aabbMax = {std::max({-1e30f, firstX, lastX}), maxHeight, std::max({-1e30f, firstZ, lastZ})};
    } else if (occlusion) {
        occlusion->heights = chunk.occlusion->heights;
    }
    if (chunk.topologyDirty) {
        for (size_t lodIndex = 0; lodIndex < lodCount; ++lodIndex) {
            const size_t firstIndex = scratch.indices.size();
            BuildTerrainPatchLodIndices(terrain, firstColumn, firstRow, lastColumn, lastRow, renderer::TERRAIN_LOD_STEPS[lodIndex], scratch.indices);
            chunk.indexCountLOD[lodIndex] = static_cast<uint32_t>(scratch.indices.size() - firstIndex);
            if (occlusion) {
                auto& cpuIndices = occlusion->indices[lodIndex];
                cpuIndices.reserve(chunk.indexCountLOD[lodIndex]);
                for (size_t i = firstIndex; i < scratch.indices.size(); ++i)
                    cpuIndices.push_back(static_cast<uint16_t>(scratch.indices[i]));
            }
        }
    } else if (occlusion) {
        /// @note 公開済みのビューが旧スナップショットを読み得るため、その高さ配列とトポロジー配列は変更しない。
        occlusion->indices = chunk.occlusion->indices;
    }
    if (occlusion && chunk.indexCountLOD[0] == uint32_t((lastColumn - firstColumn) * (lastRow - firstRow) * 6)) {
        if (!chunk.verticesDirty && !chunk.occlusion->columns.empty()) {
            occlusion->columns = chunk.occlusion->columns;
        } else {
            if (!chunk.verticesDirty) FillChunkHeights(terrain, neighbors, firstColumn, firstRow, lastColumn, lastRow, overlap, scratch);
            constexpr int columnStep = 16;
            /// @note 列の境界を最大 LOD の格子に揃え、内側へ張り出す三角形の頂点を高さ下限に含める。
            for (int z = firstRow; z < lastRow; z += columnStep) {
                for (int x = firstColumn; x < lastColumn; x += columnStep) {
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

/// @note レンダラーを待つ前に CPU 形状を収集し、解放と差し替えを同じ順序付き操作で行う。
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
            for (size_t lodIndex = 0; lodIndex < lodCount; ++lodIndex) {
                auto& buffer = chunk.indexBufferLOD[lodIndex];
                resources.Release(buffer);
                buffer = {};
                /// @note 全セルが穴のチャンクはインデックス 0 本。空バッファは作らず、描画側で飛ばす。
                const uint32_t indexCount = chunk.indexCountLOD[lodIndex];
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
    /// @note LOD 0 の非空セルは各 6 インデックスなので、不足分があればこの格子は穴を含む。
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

/// @note 洞窟の入口、壁、薄い天井を保つため、表面パッチは全 LOD で元のトポロジーを使う。
static TerrainChunk& EnsureSurfaceChunk(TerrainGpuState& gpu, const TerrainComponent& terrain, size_t firstIndex,
    renderer::ResourceManager& resources)
{
    const TerrainChunkKey key{UINT32_MAX, static_cast<uint32_t>(firstIndex / TERRAIN_SURFACE_PATCH_INDICES)};
    const auto found = gpu.chunks.find(key);
    if (found != gpu.chunks.end()) return found->second;
    const auto surfacePatch = BuildTerrainSurfacePatch(terrain, firstIndex);
    TerrainChunk chunk;
    chunk.aabbMin = surfacePatch.aabbMin; chunk.aabbMax = surfacePatch.aabbMax;
    chunk.vertexBuffer = resources.CreateVertexBuffer(surfacePatch.vertices.data(), surfacePatch.vertices.size() * sizeof(TerrainVertex), sizeof(TerrainVertex));
    for (size_t lodIndex = 0; lodIndex < renderer::TERRAIN_FAR_LOD_INDEX; ++lodIndex) {
        chunk.indexCountLOD[lodIndex] = static_cast<uint32_t>(surfacePatch.indices.size());
        chunk.indexBufferLOD[lodIndex] = resources.CreateIndexBuffer(surfacePatch.indices.data(), chunk.indexCountLOD[lodIndex]);
    }
    return gpu.chunks.emplace(key, std::move(chunk)).first->second;
}

}

void PrepareTerrainPatches(TerrainGpuState& gpu, const TerrainComponent& terrain, const TerrainNeighbors& neighbors,
    renderer::RenderTerrainInput& input, TerrainChunkScratch& scratch, const RenderPassContext& context)
{
    const size_t lodCount = input.LODCount();
    const int chunkCountX = (terrain.columns - 1 + terrain.chunkSize - 1) / terrain.chunkSize;
    const int chunkCountZ = (terrain.rows - 1 + terrain.chunkSize - 1) / terrain.chunkSize;
    const bool useRasterStrip = context.settings.rasterGeometryMode == renderer::RasterGeometryMode::TRIANGLE_STRIP;
    /// @note チャンク追加で再ハッシュが起きても、キャッシュ要素への参照は有効。
    scratch.drawChunks.clear();
    for (int chunkZ = 0; chunkZ < chunkCountZ; ++chunkZ) {
        for (int chunkX = 0; chunkX < chunkCountX; ++chunkX) {
            const auto& chunk = PrepareTerrainChunk(gpu, terrain, chunkX, chunkZ, neighbors, lodCount, scratch);
            if (chunk.indexCountLOD[0] != 0) scratch.drawChunks.push_back(&chunk);
            if (scratch.vertices.size() * sizeof(TerrainVertex) + scratch.indices.size() * sizeof(uint32_t)
                >= TERRAIN_CHUNK_BATCH_BYTES) UploadTerrainChunks(scratch, lodCount, context.renderer, context.resources);
        }
    }
    UploadTerrainChunks(scratch, lodCount, context.renderer, context.resources);
    input.patches.reserve(scratch.drawChunks.size());
    for (const auto* chunk : scratch.drawChunks) {
        auto& patch = input.patches.emplace_back(*chunk);
        if (useRasterStrip) patch.lod0Strip = EnsureTerrainStripIndices(gpu, *chunk,
            static_cast<uint32_t>(terrain.chunkSize) + 1u, context.resources);
    }
}

void ExtractTerrainSurface(TerrainRenderCache& cache, const TerrainComponent& terrain, TerrainGpuState& gpu,
    const renderer::RenderTerrainInput& base, renderer::ResourceManager& resources, renderer::RenderScene& output)
{
    if (terrain.surfaceIndices.empty() || !IsValidTerrainSurfaceGeometry(terrain.surfacePositions, terrain.surfaceIndices)) return;
    renderer::RenderTerrainInput surface;
    surface.layer = base.layer; surface.selected = base.selected;
    surface.chunkWorldSize = base.chunkWorldSize;
    surface.constants = base.constants;
    surface.constants.grassGroundColor = {};
    surface.splatIndices = base.splatIndices; surface.splatWeights = base.splatWeights;
    if (!terrain.surfaceMaterial.empty() && !ApplyTerrainSurfaceMaterial(cache, terrain, gpu, surface, resources)) return;
    for (size_t firstIndex = 0; firstIndex < terrain.surfaceIndices.size(); firstIndex += TERRAIN_SURFACE_PATCH_INDICES)
        surface.patches.push_back(EnsureSurfaceChunk(gpu, terrain, firstIndex, resources));
    /// @note 洞窟の天井は高さ場として GPU で復号せず、地形の草も生成しない。カメラ、影、マスクは同じ表面パッチを使う。
    output.terrains.push_back(std::move(surface));
}

/// @note LOD0 の水平位置と索引は同じ格子、穴、パッチ範囲から生成する。高さは接合後の描画値を使う。
uint64_t TerrainShadowBakeGeometryHash(const TerrainComponent& terrain,
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

}
