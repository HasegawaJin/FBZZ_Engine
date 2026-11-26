/// @file    RenderWaterMesh.cpp
/// @brief   水面形状ごとのメッシュと描画用インデックスの生成。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "RenderWaterResources.hpp"
#include <Engine/Scene/Components/Water/WaterComponent.hpp>
#include <Engine/Scene/Components/Water/WaterZoneComponent.hpp>
#include <Engine/Scene/Water/Surface/WaterSurfaceQuery.hpp>
#include <Engine/Scene/Water/Surface/WaterSpline.hpp>
#include <Engine/Scene/Water/Surface/WaterZoneSnapshot.hpp>
#include <Graphics/Renderer/Resources/ResourceManager.hpp>
#include <Graphics/Renderer/Geometry/SurfacePatchLod.hpp>
#include <Math/Scalar/MathUtils.hpp>
#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <span>
#include <utility>

namespace fbzz::scene::water_render_detail {

namespace {

uint32_t WaterChunkCount(const WaterComponent& water)
{
    return (std::min)((std::max)(water.chunkCount, 1u), (std::min)(water.resolutionX, water.resolutionZ));
}

uint32_t WaterChunkCellOffset(uint32_t cellsPerAxis, uint32_t chunkIndex, uint32_t chunksPerAxis)
{
    return static_cast<uint32_t>((static_cast<uint64_t>(cellsPerAxis) * chunkIndex) / chunksPerAxis);
}

std::shared_ptr<WaterPatchGrid> AcquireWaterPatchGrid(uint32_t cellsPerPatch, WaterPatchGridSlots& patchGridSlots,
    renderer::ResourceManager& resources)
{
    assert(cellsPerPatch >= 2u && cellsPerPatch <= 64u && std::has_single_bit(cellsPerPatch));
    auto& cachedGrid = patchGridSlots[std::bit_width(cellsPerPatch) - 2u];
    if (auto grid = cachedGrid.lock()) {
        return grid;
    }
    auto grid = std::make_shared<WaterPatchGrid>(resources);
    std::vector<renderer::WaterVertex> vertices;
    vertices.reserve(static_cast<size_t>(cellsPerPatch + 1u) * (cellsPerPatch + 1u));
    for (uint32_t vertexRow = 0; vertexRow <= cellsPerPatch; ++vertexRow) {
        for (uint32_t vertexColumn = 0; vertexColumn <= cellsPerPatch; ++vertexColumn) {
            const math::Vector2 uv{static_cast<float>(vertexColumn) / cellsPerPatch, static_cast<float>(vertexRow) / cellsPerPatch};
            vertices.push_back({{uv.x, 0.0f, uv.y}, uv});
        }
    }
    grid->vertexBuffer = resources.CreateVertexBuffer(vertices.data(), vertices.size() * sizeof(renderer::WaterVertex),
        static_cast<uint32_t>(sizeof(renderer::WaterVertex)));
    if (!grid->vertexBuffer.IsValid()) {
        return grid;
    }
    std::vector<uint32_t> indices;
    for (uint8_t coarseEdgeMask = 0; coarseEdgeMask < grid->indexBuffers.size(); ++coarseEdgeMask) {
        if (!renderer::BuildSurfacePatchIndices(cellsPerPatch, coarseEdgeMask, indices)) {
            continue;
        }
        grid->indexCounts[coarseEdgeMask] = static_cast<uint32_t>(indices.size());
        grid->indexBuffers[coarseEdgeMask] = resources.CreateIndexBuffer(indices.data(), grid->indexCounts[coarseEdgeMask]);
    }
    // 生成に失敗したグリッドは、他の水面の生成や後続の再構築で再試行できる。
    if (std::all_of(grid->indexBuffers.begin(), grid->indexBuffers.end(),
            [](auto buffer) { return buffer.IsValid(); })) {
        cachedGrid = grid;
    }
    return grid;
}

void BuildSplineWaterMesh(const WaterComponent& water, WaterMesh& mesh,
    renderer::ResourceManager& resources, bool buildSurfaceClusters)
{
    const WaterSplineSnapshot* spline = WaterSurfaceGeometry(water, WaterSurfaceReadView::SIMULATION);
    if (!spline) {
        return;
    }
    const auto& splineData = *spline;
    mesh.aabbMin = splineData.localBoundsMin;
    mesh.aabbMax = splineData.localBoundsMax;
    const math::Vector2 localExtent = WaterSurfaceLocalExtent(water, WaterSurfaceReadView::SIMULATION);
    if (splineData.vertices.empty() || splineData.triangles.empty()
        || !std::isfinite(localExtent.x) || !std::isfinite(localExtent.y)
        || localExtent.x <= 0.0f || localExtent.y <= 0.0f) {
        return;
    }

    const auto* zoneSnapshot = water.simulationZoneSnapshot.get();
    const bool blendMaterials = zoneSnapshot && zoneSnapshot->materials.size() == splineData.vertices.size();
    mesh.surfaceClustersAttempted = buildSurfaceClusters;
    std::vector<renderer::WaterVertex> vertices;
    vertices.reserve(blendMaterials ? splineData.triangles.size() * 3u : splineData.vertices.size());
    const float depthCap = (std::max)(water.buoyancyDepth, 0.0f);
    const auto makeVertex = [&](const WaterSplineVertex& source) {
        renderer::WaterVertex vertex{};
        vertex.position = source.localPosition;
        vertex.uv = {source.localPosition.x / localExtent.x + 0.5f,
            source.localPosition.z / localExtent.y + 0.5f};
        vertex.splineData = {(std::min)(source.depth, depthCap),
            source.worldVelocity.x, source.worldVelocity.z, 1.0f + source.horizonMaterialWeight};
        vertex.riverTravelTimes = {source.riverTravelTime, source.riverShortTravelTime};
        return vertex;
    };
    const auto appendZoneTriangle = [&](std::span<const uint32_t, 3> triangle,
        std::vector<renderer::WaterVertex>& outputVertices) {
        static_assert(WATER_ZONE_MAX_BODIES == 32);
        uint32_t packedReceivers = 0, packedSources = 0;
        for (uint32_t corner = 0; corner < 3u; ++corner) {
            const auto& profile = zoneSnapshot->materials[triangle[corner]];
            packedReceivers |= profile.receiver << (corner * 5u);
            packedSources |= profile.source << (corner * 5u);
        }
        for (uint32_t corner = 0; corner < 3u; ++corner) {
            const uint32_t index = triangle[corner];
            renderer::WaterVertex vertex = makeVertex(splineData.vertices[index]);
            vertex.zoneMaterial = {static_cast<float>(packedReceivers), static_cast<float>(packedSources),
                static_cast<float>(corner + 1u), zoneSnapshot->materials[index].sourceWeight};
            outputVertices.push_back(vertex);
        }
    };
    std::vector<uint32_t> indices;
    indices.reserve(splineData.triangles.size() * 3u);
    if (blendMaterials) {
        for (const auto& triangle : splineData.triangles) {
            const auto firstVertex = static_cast<uint32_t>(vertices.size());
            indices.insert(indices.end(), {firstVertex, firstVertex + 1u, firstVertex + 2u});
            appendZoneTriangle(triangle.indices, vertices);
        }
    } else {
        for (const auto& source : splineData.vertices) {
            vertices.push_back(makeVertex(source));
        }
        for (const auto& triangle : splineData.triangles) {
            indices.insert(indices.end(), triangle.indices.begin(), triangle.indices.end());
        }
    }

    renderer::RenderWaterPatch chunk;
    chunk.vertexBuffer = resources.CreateVertexBuffer(vertices.data(),
        vertices.size() * sizeof(renderer::WaterVertex), static_cast<uint32_t>(sizeof(renderer::WaterVertex)));
    chunk.indexBuffer = resources.CreateIndexBuffer(indices.data(), static_cast<uint32_t>(indices.size()));
    chunk.indexCount = static_cast<uint32_t>(indices.size());
    chunk.aabbMin = splineData.localBoundsMin;
    chunk.aabbMax = splineData.localBoundsMax;
    mesh.chunks.push_back(std::move(chunk));
    if (!buildSurfaceClusters) {
        return;
    }

    std::vector<renderer::WaterVertex> sharedVertices;
    std::vector<uint32_t> sharedIndices;
    if (blendMaterials) {
        sharedVertices.reserve(splineData.vertices.size());
        for (const auto& source : splineData.vertices) {
            sharedVertices.push_back(makeVertex(source));
        }
        sharedIndices.reserve(indices.size());
        for (const auto& triangle : splineData.triangles) {
            sharedIndices.insert(sharedIndices.end(), triangle.indices.begin(), triangle.indices.end());
        }
    }
    std::vector<uint8_t> protectedVertices(splineData.vertices.size());
    for (const auto& triangle : splineData.triangles) {
        bool preserveTriangle = false;
        bool underDepthCap = false, overDepthCap = false;
        for (uint32_t index : triangle.indices) {
            const auto& source = splineData.vertices[index];
            preserveTriangle |= source.horizonMaterialWeight > 0.0f;
            underDepthCap |= source.depth < depthCap;
            overDepthCap |= source.depth > depthCap;
            if (blendMaterials) {
                const auto& firstProfile = zoneSnapshot->materials[triangle.indices[0]];
                const auto& cornerProfile = zoneSnapshot->materials[index];
                preserveTriangle |= firstProfile.receiver != cornerProfile.receiver
                    || firstProfile.source != cornerProfile.source || firstProfile.sourceWeight != cornerProfile.sourceWeight;
            }
        }
        if (preserveTriangle || (underDepthCap && overDepthCap))
            for (uint32_t index : triangle.indices) {
                protectedVertices[index] = 1u;
            }
    }
    auto surfaceClusters = std::make_shared<std::vector<renderer::WaterSurfaceCluster>>();
    std::vector<uint32_t> clusterIndices;
    if (!renderer::BuildWaterSurfaceClusters(
            blendMaterials ? sharedVertices : vertices, blendMaterials ? sharedIndices : indices,
            protectedVertices, *surfaceClusters, clusterIndices)) {
        return;
    }
    if (blendMaterials) {
        // 複数の材質を含む元の三角形は 3 頂点すべての属性を使う。均一な材質領域に追加する三角形でも頂点ごとの補間は必要。
        std::vector<renderer::WaterVertex> clusterVertices;
        clusterVertices.reserve(clusterIndices.size());
        for (size_t triangleOffset = 0; triangleOffset < clusterIndices.size(); triangleOffset += 3) {
            const std::span<uint32_t, 3> triangle(clusterIndices.data() + triangleOffset, 3u);
            appendZoneTriangle(triangle, clusterVertices);
            for (uint32_t corner = 0; corner < 3u; ++corner) {
                triangle[corner] = static_cast<uint32_t>(triangleOffset) + corner;
            }
        }
        mesh.surfaceClusterVertices = resources.CreateVertexBuffer(clusterVertices.data(),
            clusterVertices.size() * sizeof(renderer::WaterVertex), static_cast<uint32_t>(sizeof(renderer::WaterVertex)));
        if (!mesh.surfaceClusterVertices.IsValid()) {
            return;
        }
    }
    mesh.surfaceClusterIndices = resources.CreateIndexBuffer(clusterIndices.data(),
        static_cast<uint32_t>(clusterIndices.size()));
    if (mesh.surfaceClusterIndices.IsValid()) {
        mesh.surfaceClusters = std::move(surfaceClusters);
    }
}

void BuildRectangularWaterMesh(const WaterComponent& water, WaterMesh& mesh,
    std::shared_ptr<WaterPatchGrid> patchGrid, renderer::ResourceManager& resources)
{
    const float halfExtentX = water.extentX * 0.5f;
    const float halfExtentZ = water.extentZ * 0.5f;
    mesh.aabbMin = { -halfExtentX, 0.0f, -halfExtentZ };
    mesh.aabbMax = { halfExtentX, 0.0f, halfExtentZ };
    if (patchGrid) {
        mesh.patchGrid = std::move(patchGrid);
        return;
    }

    const float cellWidth = water.extentX / static_cast<float>(water.resolutionX);
    const float cellDepth = water.extentZ / static_cast<float>(water.resolutionZ);
    const float localOriginX = -water.extentX * 0.5f;
    const float localOriginZ = -water.extentZ * 0.5f;

    // 境界を按分して、割り切れない解像度でも最後のチャンクがセル総数を超えないようにする。
    const uint32_t chunksPerAxis = WaterChunkCount(water);

    for (uint32_t chunkZ = 0; chunkZ < chunksPerAxis; ++chunkZ) {
        for (uint32_t chunkX = 0; chunkX < chunksPerAxis; ++chunkX) {
            const uint32_t firstCellX = WaterChunkCellOffset(water.resolutionX, chunkX, chunksPerAxis);
            const uint32_t endCellX   = WaterChunkCellOffset(water.resolutionX, chunkX + 1u, chunksPerAxis);
            const uint32_t firstCellZ = WaterChunkCellOffset(water.resolutionZ, chunkZ, chunksPerAxis);
            const uint32_t endCellZ   = WaterChunkCellOffset(water.resolutionZ, chunkZ + 1u, chunksPerAxis);
            if (endCellX <= firstCellX || endCellZ <= firstCellZ) {
                continue;
            }

            const uint32_t vertexColumns = endCellX - firstCellX + 1;
            const uint32_t vertexRows = endCellZ - firstCellZ + 1;

            std::vector<renderer::WaterVertex> vertices;
            vertices.reserve(static_cast<size_t>(vertexColumns) * static_cast<size_t>(vertexRows));
            for (uint32_t cellZ = firstCellZ; cellZ <= endCellZ; ++cellZ) {
                for (uint32_t cellX = firstCellX; cellX <= endCellX; ++cellX) {
                    renderer::WaterVertex vertex;
                    vertex.position = { localOriginX + static_cast<float>(cellX) * cellWidth,
                        0.0f, localOriginZ + static_cast<float>(cellZ) * cellDepth };
                    vertex.uv = {
                        static_cast<float>(cellX) / static_cast<float>(water.resolutionX),
                        static_cast<float>(cellZ) / static_cast<float>(water.resolutionZ)
                    };
                    vertices.push_back(vertex);
                }
            }

            std::vector<uint32_t> indices;
            indices.reserve(static_cast<size_t>(vertexColumns - 1) * static_cast<size_t>(vertexRows - 1) * 6u);
            for (uint32_t cellZ = 0; cellZ < vertexRows - 1; ++cellZ) {
                for (uint32_t cellX = 0; cellX < vertexColumns - 1; ++cellX) {
                    const uint32_t topLeft = cellZ * vertexColumns + cellX;
                    const uint32_t topRight = topLeft + 1;
                    const uint32_t bottomLeft = topLeft + vertexColumns;
                    const uint32_t bottomRight = bottomLeft + 1;
                    indices.insert(indices.end(), { topLeft, bottomLeft, topRight, topRight, bottomLeft, bottomRight });
                }
            }

            renderer::RenderWaterPatch chunk;
            chunk.vertexBuffer = resources.CreateVertexBuffer(
                vertices.data(), vertices.size() * sizeof(renderer::WaterVertex), static_cast<uint32_t>(sizeof(renderer::WaterVertex)));
            chunk.indexBuffer = resources.CreateIndexBuffer(
                indices.data(), static_cast<uint32_t>(indices.size()));
            chunk.indexCount = static_cast<uint32_t>(indices.size());
            chunk.aabbMin = { localOriginX + static_cast<float>(firstCellX) * cellWidth,
                0.0f, localOriginZ + static_cast<float>(firstCellZ) * cellDepth };
            chunk.aabbMax = { localOriginX + static_cast<float>(endCellX) * cellWidth,
                0.0f, localOriginZ + static_cast<float>(endCellZ) * cellDepth };
            mesh.chunks.push_back(std::move(chunk));
        }
    }

    mesh.aabbMin = { localOriginX, 0.0f, localOriginZ };
    mesh.aabbMax = { -localOriginX, 0.0f, -localOriginZ };
}

}

void NormalizeWaterMeshSettings(WaterComponent& water, bool splineSurface)
{
    water.resolutionX = std::clamp(water.resolutionX, 1u, 512u);
    water.resolutionZ = std::clamp(water.resolutionZ, 1u, 512u);
    water.chunkCount  = std::clamp(water.chunkCount,  1u, 64u);
    if (!splineSurface) {
        water.extentX = std::clamp(water.extentX, 0.1f, 10000.0f);
        water.extentZ = std::clamp(water.extentZ, 0.1f, 10000.0f);
    }
    water.lodPatchResolution = std::bit_ceil((std::clamp)(water.lodPatchResolution, 2u, 64u));
}

WaterMeshGeometry ResolveWaterMeshGeometry(const WaterComponent& water)
{
    WaterMeshGeometry geometryKey;
    geometryKey.surfaceShape = WaterSurfaceEffectiveShape(water, WaterSurfaceReadView::SIMULATION);
    if (UsesWaterSurfaceGeometry(water, WaterSurfaceReadView::SIMULATION)) {
        const auto* spline = WaterSurfaceGeometry(water, WaterSurfaceReadView::SIMULATION);
        geometryKey.splineSignature = spline ? spline->signature : 0;
        geometryKey.splineDepthCap = (std::max)(water.buoyancyDepth, 0.0f);
        return geometryKey;
    }
    geometryKey.extentX = water.extentX;
    geometryKey.extentZ = water.extentZ;
    geometryKey.sharedPatchGrid = water.adaptivePatchLod || water.farOceanEnabled;
    if (geometryKey.sharedPatchGrid) {
        geometryKey.patchResolution = water.lodPatchResolution;
    }
    else {
        geometryKey.resolutionX = water.resolutionX;
        geometryKey.resolutionZ = water.resolutionZ;
        geometryKey.chunkCount = water.chunkCount;
    }
    return geometryKey;
}

/// @note Gerstner 変位の上限は、水平が Q*A、垂直が A の総和。
renderer::WaterWaveMargin ComputeWaveMargin(const WaterComponent& water, bool horizonSurface)
{
    renderer::WaterWaveMargin margin;
    if (water.enableGerstnerWaves) {
        // 群の包絡は最大 (1 + waveGrouping) 倍、方向広がりは主 + 伴走の和まで振幅を持ち上げる。
        // ここに入れ忘れると、山に当たったチャンクだけが «AABB からはみ出した» 扱いで消える。
        const float amplitudeMultiplier = (1.0f + math::Clamp01(water.waveGrouping))
                         * WaterComponent::WaveSpreadAmplitudeSum(water.waveSpread);
        for (size_t waveIndex = 0; waveIndex < (std::min)(static_cast<size_t>(water.waveCount), water.waves.size()); ++waveIndex) {
            const GerstnerWave& wave = water.waves[waveIndex];
            const float amplitude = (std::max)(wave.amplitude, 0.0f) * amplitudeMultiplier;
            margin.vertical   += amplitude;
            margin.horizontal += amplitude * math::Clamp01(wave.steepness);
        }
        // 水平海面との交差補正は高さだけを加え、水平方向の境界は変えない。
        if (horizonSurface) {
            margin.vertical += margin.vertical * WATER_CROSSING_CREST_STRENGTH;
        }
    }
    // 流れの場が作る形と着水の輪も «平面から出る量»。入れないと穴や山や輪の上のチャンク
    // だけが «AABB からはみ出した» 扱いで消える。盛り上がりも穴と同じ枠で測る。
    float flowDisplacement = 0.0f;
    for (int flowIndex = 0; flowIndex < (std::min)(water.surfaceFlowCount, kWaterSurfaceFlowCount); ++flowIndex) {
        // 重複する場の変位は加算されるため、各場の変位幅を水面の上下限に含める。
        flowDisplacement += std::abs(water.surfaceFlows[static_cast<size_t>(flowIndex)].height);
    }
    // 輪の合成断面の上限は本体 + 後続 2 本 = 1.58 倍。
    margin.vertical += flowDisplacement
                     + (water.ripples.empty() ? 0.0f : kWaterRippleHeightAmplitude * 1.6f);

    // さざ波と描画専用の河川波頭（上下 0.15 m 以下）を既存の余裕に収める。
    margin.horizontal += 0.5f;
    margin.vertical   += 0.5f;
    return margin;
}

void BuildWaterMesh(const WaterComponent& water, WaterMesh& mesh, WaterPatchGridSlots& patchGridSlots,
    renderer::ResourceManager& resources, bool buildSurfaceClusters)
{
    assert(water.resolutionX > 0);
    assert(water.resolutionZ > 0);

    const WaterMeshGeometry geometryKey = ResolveWaterMeshGeometry(water);
    // 同じ分割数のまま外形を編集したときも、差し替えまで既存グリッドを保持する。
    auto patchGrid = geometryKey.sharedPatchGrid ? AcquireWaterPatchGrid(geometryKey.patchResolution, patchGridSlots, resources)
        : nullptr;
    ReleaseWaterMesh(mesh, resources);
    mesh.geometry = geometryKey;

    if (UsesWaterSurfaceGeometry(water, WaterSurfaceReadView::SIMULATION)) {
        BuildSplineWaterMesh(water, mesh, resources, buildSurfaceClusters);
    } else {
        BuildRectangularWaterMesh(water, mesh, std::move(patchGrid), resources);
    }
}

void PopulateWaterGeometry(renderer::RenderWaterInput& input, const WaterComponent& water,
    const WaterMesh& mesh, bool splineSurface, bool horizonSurface, bool buildSurfaceClusters)
{
    input.aabbMin = mesh.aabbMin;
    input.aabbMax = mesh.aabbMax;
    input.patches = mesh.chunks;
    if (buildSurfaceClusters) {
        input.surfaceClusters = mesh.surfaceClusters;
        input.surfaceClusterIndices = mesh.surfaceClusterIndices;
        input.surfaceClusterVertices = mesh.surfaceClusterVertices;
        if (water.adaptivePatchLod && std::isfinite(water.lodTargetPixels)) {
            input.surfaceClusterErrorPixels = std::clamp(water.lodTargetPixels, 1.0f, 8.0f);
        }
    }
    input.adaptivePatchLod = !splineSurface && (water.adaptivePatchLod || water.farOceanEnabled);
    input.patchMorphEnabled = !splineSurface && water.lodMorphEnabled;
    input.farOceanEnabled = horizonSurface;
    input.farOceanCoverageScale = water.farOceanCoverageScale;
    input.farOceanRippleFadeDistance = water.farOceanRippleFadeDistance;
    input.patchLod.cellsPerPatch = water.lodPatchResolution;
    input.patchLod.maxDepth = (std::min)(water.lodMaxDepth, 10u);
    input.patchLod.maxPatches = (std::clamp)(water.lodMaxPatches, 1u, 4096u);
    input.patchLod.targetCellPixels = water.lodTargetPixels;
    input.patchLod.nearDistance = water.lodNearDistance;
    input.patchLod.nearCellSize = water.lodNearCellSize;
    input.patchLod.morphEnabled = !splineSurface && water.lodMorphEnabled;
    input.patchLod.morphTransitionWidth = water.lodMorphTransitionWidth;
    if (const auto& grid = mesh.patchGrid) {
        input.patchVertexBuffer = grid->vertexBuffer;
        input.patchIndexBuffers = grid->indexBuffers;
        input.patchIndexCounts = grid->indexCounts;
        input.patchStrips = grid->strips;
    }
}

void PrepareWaterStripBuffers(const WaterComponent& water, WaterMesh& mesh, renderer::ResourceManager& resources)
{
    bool stripsReady = true;
    std::vector<uint32_t> indices;
    const auto uploadStripIndices = [&](renderer::WaterStripIndices& strip) {
        strip.count = static_cast<uint32_t>(indices.size());
        strip.buffer = resources.CreateIndexBuffer(indices.data(), strip.count);
        stripsReady &= strip.buffer.IsValid();
    };
    if (mesh.patchGrid) {
        auto& grid = *mesh.patchGrid;
        for (uint8_t coarseEdgeMask = 0; coarseEdgeMask < grid.strips.size(); ++coarseEdgeMask) {
            auto& strip = grid.strips[coarseEdgeMask];
            if (strip.buffer.IsValid()) {
                continue;
            }
            if (!renderer::BuildSurfacePatchStripIndices(water.lodPatchResolution, coarseEdgeMask, indices)) {
                stripsReady = false;
                continue;
            }
            uploadStripIndices(strip);
        }
        grid.stripsReady = stripsReady;
        return;
    } else {
        const uint32_t chunksPerAxis = WaterChunkCount(water);
        if (mesh.chunks.size() != static_cast<size_t>(chunksPerAxis) * chunksPerAxis) {
            return;
        }
        for (uint32_t chunkZ = 0; chunkZ < chunksPerAxis; ++chunkZ) {
            const uint32_t cellsZ = WaterChunkCellOffset(water.resolutionZ, chunkZ + 1u, chunksPerAxis)
                - WaterChunkCellOffset(water.resolutionZ, chunkZ, chunksPerAxis);
            for (uint32_t chunkX = 0; chunkX < chunksPerAxis; ++chunkX) {
                auto& strip = mesh.chunks[chunkZ * chunksPerAxis + chunkX].strip;
                if (strip.buffer.IsValid()) {
                    continue;
                }
                const uint32_t cellsX = WaterChunkCellOffset(water.resolutionX, chunkX + 1u, chunksPerAxis)
                    - WaterChunkCellOffset(water.resolutionX, chunkX, chunksPerAxis);
                if (!renderer::BuildRegularSurfaceStripIndices(cellsX, cellsZ, indices)) {
                    stripsReady = false;
                    continue;
                }
                uploadStripIndices(strip);
            }
        }
    }
    mesh.stripsReady = stripsReady;
}

}
