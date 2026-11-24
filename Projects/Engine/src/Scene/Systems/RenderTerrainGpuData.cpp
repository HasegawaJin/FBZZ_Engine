/// @file    RenderTerrainGpuData.cpp
/// @brief   地形の GPU 描画用高さ・索引とスプラットテクスチャの更新。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "RenderTerrainExtraction.hpp"
#include "RenderPasses/Geometry/TerrainPatchIndices.hpp"
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/IStructuredBuffer.hpp>
#include <Graphics/Renderer/ITexture.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Core/Profiler/ProfileScope.hpp>
#include <algorithm>

namespace fbzz::scene::terrain_extraction_detail {

namespace {

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

static void UpdateTerrainGpuHeights(TerrainRenderCache& cache, TerrainGpuState& state, const TerrainComponent& terrain,
    const TerrainNeighbors& neighbors, renderer::IRenderer& renderer, renderer::ResourceManager& resources)
{
    if (!state.heightsDirty && state.heightUploadRegion.Empty() && resources.Get(state.heights)) return;
    FBZZ_PROFILE_SCOPE("Terrain::HeightUpload");
    const size_t paddedRowStride = static_cast<size_t>(terrain.columns) + 2;
    const size_t paddedSampleCount = paddedRowStride * (static_cast<size_t>(terrain.rows) + 2);
    const auto* existingHeightBuffer = resources.Get(state.heights);
    const bool sameGridSize = existingHeightBuffer && existingHeightBuffer->GetElementCount() == paddedSampleCount;
    const auto& region = state.heightUploadRegion;
    const bool hasPartialHeightUpdate = !state.heightsDirty && !region.Empty() && sameGridSize;
    if (hasPartialHeightUpdate && state.heightsDynamic) {
        const size_t width = static_cast<size_t>(region.maxX - region.minX + 1);
        const size_t rows = static_cast<size_t>(region.maxZ - region.minZ + 1);
        std::vector<float> heights(width * rows);
        for (int z = region.minZ; z <= region.maxZ; ++z)
            FillTerrainHeightRow(terrain, neighbors, region.minX, region.maxX, z,
                heights.data() + static_cast<size_t>(z - region.minZ) * width);
        const size_t firstSample = static_cast<size_t>(region.minZ + 1) * paddedRowStride + region.minX + 1;
        bool uploaded = false;
        /// @note 既存の描画に続けて既定ヒープのバッファコピーを実行し、CPU 側の同期をまとめる。
        renderer.ExecuteResourceOperation([&] {
            if (auto* buffer = resources.Get(state.heights))
                uploaded = buffer->UpdateRows(heights.data(), width * sizeof(float), rows,
                    paddedRowStride * sizeof(float), firstSample * sizeof(float));
        });
        if (uploaded) {
            state.heightUploadRegion = {};
            return;
        }
    }

    std::vector<float> heights(paddedSampleCount);
    for (int z = -1; z <= terrain.rows; ++z)
        FillTerrainHeightRow(terrain, neighbors, -1, terrain.columns, z,
            heights.data() + static_cast<size_t>(z + 1) * paddedRowStride);
    bool reused = false;
    if (state.heightsDynamic && sameGridSize)
        reused = resources.UpdateRegion(state.heights, heights.data(), heights.size() * sizeof(float), 0);
    if (!reused) {
        resources.Release(state.heights);
        state.heights = {};
        state.heightsDynamic = false;
        if (hasPartialHeightUpdate && !cache.heightDynamicUnavailable) {
            renderer.ExecuteResourceOperation([&] {
                const auto writable = resources.CreateRWStructuredBuffer(nullptr, static_cast<uint32_t>(paddedSampleCount), sizeof(float));
                if (resources.UpdateRegion(writable, heights.data(), heights.size() * sizeof(float), 0)) {
                    state.heights = writable;
                    state.heightsDynamic = true;
                } else {
                    resources.Release(writable);
                }
            });
            if (!state.heightsDynamic) cache.heightDynamicUnavailable = true;
        }
        /// @note 矩形の一部だけを転送準備した場合も含め、順序を保証できなければ不変の SRV を差し替える。
        if (!state.heightsDynamic)
            state.heights = resources.CreateStructuredBuffer(heights.data(), static_cast<uint32_t>(paddedSampleCount), sizeof(float));
    }
    state.heightsDirty = !resources.Get(state.heights);
    state.heightUploadRegion = {};
}

}

void UpdateSplatTextures(TerrainRenderCache& cache, TerrainGpuState& gpu, TerrainComponent& terrain,
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
    const auto& dirtyRegion = preview.region;
    const bool hasPartialSplatUpdate = !dirtyRegion.Empty() && preview.revision == terrain.vegetationRevision
        && dirtyRegion.minX >= 0 && dirtyRegion.minZ >= 0 && dirtyRegion.maxX < terrain.columns && dirtyRegion.maxZ < terrain.rows;
    const TerrainHeightRegion fullRegion{0, 0, terrain.columns - 1, terrain.rows - 1};
    const bool canCreateDynamicTextures = hasSplat && hasPartialSplatUpdate && !gpu.splatDynamic && !cache.splatDynamicUnavailable;
    /// @note 古いビューが新旧のマップを混ぜて読まないよう、2 枚を一度の順序付き操作で投入する。
    renderer.ExecuteResourceOperation([&] {
        if (hasSplat && gpu.splatDynamic) {
            const auto& region = hasPartialSplatUpdate && gpu.vegetationRevision == preview.baseRevision ? dirtyRegion : fullRegion;
            if (UploadSplatRectangle(resources, gpu.splatIndices, gpu.splatWeights, terrain, region)) return;
        }
        /// @note 最初の局所編集でタイルを更新可能な表現へ切り替え、未編集のタイルは元の不変テクスチャを保つ。
        if (canCreateDynamicTextures) {
            const auto width = static_cast<uint32_t>(terrain.columns);
            const auto height = static_cast<uint32_t>(terrain.rows);
            const auto indices = resources.CreateDynamicTexture(width, height, renderer::DynamicTextureFormat::RGBA8);
            const auto weights = indices.IsValid()
                ? resources.CreateDynamicTexture(width, height, renderer::DynamicTextureFormat::RGBA8)
                : renderer::ResourceHandle<renderer::TextureTag>{};
            if (UploadSplatRectangle(resources, indices, weights, terrain, fullRegion)) {
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

void PrepareTerrainGpuGeometry(TerrainGpuState& state, const TerrainComponent& terrain,
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
            for (size_t lodIndex = 0; lodIndex < input.LODCount(); ++lodIndex) indexCount += chunk.indexCountLOD[lodIndex];
        indices.reserve(indexCount);
        for (const auto& chunk : input.patches) {
            const auto& patch = chunk.gpuPatch;
            patches.push_back(patch);
            decltype(chunk.gpuStartIndexLOD) lodIndexStarts{};
            for (size_t lodIndex = 0; lodIndex < input.LODCount(); ++lodIndex) {
                lodIndexStarts[lodIndex] = static_cast<uint32_t>(indices.size());
                if (chunk.occlusion) {
                    const auto& cpuIndices = chunk.occlusion->indices[lodIndex];
                    indices.insert(indices.end(), cpuIndices.begin(), cpuIndices.end());
                } else {
                    BuildTerrainPatchLodIndices(terrain, static_cast<int>(patch.originX), static_cast<int>(patch.originZ),
                        static_cast<int>(patch.originX + patch.columns - 1),
                        static_cast<int>(patch.originZ + patch.rows - 1), renderer::TERRAIN_LOD_STEPS[lodIndex], indices);
                }
            }
            state.indexStarts.push_back(lodIndexStarts);
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

}
