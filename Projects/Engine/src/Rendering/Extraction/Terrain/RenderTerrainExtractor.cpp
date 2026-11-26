/// @file    RenderTerrainExtractor.cpp
/// @brief   地形の描画入力を組み立てる。
/// @author  Hasegawa Jin
/// @date    2026-06-18

#include "../RenderCandidateLease.hpp"
#include "../RenderExtractionPhases.hpp"
#include "RenderTerrainExtraction.hpp"
#include "../../Passes/Debug/SelectionPasses.hpp"
#include "../../Passes/Geometry/GeometryPasses.hpp"
#include <Engine/Rendering/Extraction/Terrain/RenderTerrainExtractor.hpp>
#include <Engine/Rendering/Passes/RenderPassContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/VFX/FiberComponent.hpp>
#include <Engine/Scene/Components/Water/WaterComponent.hpp>
#include <Engine/Scene/Water/Surface/WaterExclusion.hpp>
#include <Engine/Scene/Water/Surface/WaterSurfaceQuery.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Core/Profiler/ProfileScope.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <optional>
#include <utility>

namespace fbzz::scene {

namespace {

float ResolveTerrainFarLodDistance(const TerrainComponent& terrain)
{
    return std::isfinite(terrain.farLodDistance) ? (std::max)(terrain.farLodDistance, 0.0f) : 0.0f;
}

void ApplyTerrainFiberSettings(Scene& scene, EntityID entity, uint32_t layerCount,
    std::optional<math::Vector4>& grassWaterClip, renderer::RenderTerrainInput& input)
{
    const auto* fiber = scene.GetComponent<FiberComponent>(entity);
    input.fiberSurface = fiber && fiber->m_enabled && !fiber->m_materialPath.empty();
    if (!input.fiberSurface) {
        return;
    }

    // 草の描画距離は常に渡し、遠景を地表色へ置き換える設定だけを追加で検査する。
    input.fiberDistanceLod = fiber->m_distanceLod;
    input.fiberLodFar = std::isfinite(fiber->m_lodFar) ? std::max(fiber->m_lodFar, 0.1f) : 40.0f;
    if (fiber->m_mode != FiberRenderMode::BLADE || !fiber->m_distanceLod) {
        return;
    }
    const bool hasBlades = fiber->m_bladeDensity > 0.0f && fiber->m_densityScale > 0.0f && fiber->m_lengthScale > 0.0f;
    if (!hasBlades || fiber->m_terrainLayer >= static_cast<int>(layerCount)) {
        return;
    }
    if (!std::isfinite(fiber->m_lodNear) || !std::isfinite(fiber->m_lodFar)) {
        return;
    }
    const auto& groundColor = fiber->m_terrainGroundColor;
    const bool validGroundCover = std::isfinite(fiber->m_terrainGroundCover) && fiber->m_terrainGroundCover > 0.0f
        && std::isfinite(groundColor.x) && std::isfinite(groundColor.y) && std::isfinite(groundColor.z);
    if (!validGroundCover) {
        return;
    }

    // 地表色を適用する LOD の区間とレイヤーしきい値を揃え、水中には適用しないようにする。
    const float nearDistance = std::max(fiber->m_lodNear, 0.0f);
    const float farDistance = std::max(fiber->m_lodFar, nearDistance + 0.1f);
    const float sparseStart = fiber->m_terrainSparseLodStart;
    const float fullDistance = sparseStart > nearDistance && sparseStart < farDistance ? sparseStart : farDistance;
    const float threshold = std::isfinite(fiber->m_terrainLayerThreshold)
        ? std::clamp(fiber->m_terrainLayerThreshold, 0.0f, 1.0f) : 0.0f;
    if (!grassWaterClip) {
        grassWaterClip = GetTerrainGrassWaterClip(scene);
    }
    input.constants.grassGroundWaterClip = *grassWaterClip;
    input.constants.grassGroundLod = {nearDistance, fullDistance, static_cast<float>(fiber->m_terrainLayer), threshold};
    input.constants.grassGroundColor = {std::clamp(groundColor.x, 0.0f, 1.0f), std::clamp(groundColor.y, 0.0f, 1.0f),
        std::clamp(groundColor.z, 0.0f, 1.0f), std::clamp(fiber->m_terrainGroundCover, 0.0f, 1.0f)};
}

}

math::Vector4 GetTerrainGrassWaterClip(Scene& scene)
{
    math::Vector4 terrainWaterClip{};
    for (EntityID id : scene.GetEntities<WaterComponent>()) {
        const auto* owner = scene.GetGameObject(id);
        const auto* water = scene.GetComponent<WaterComponent>(id);
        if (!owner || !owner->activeInHierarchy() || !water || !water->enabled
            || !WaterSurfaceUsesHorizon(*water) || !IsWaterSurfaceSupported(*water, owner->transform)) {
            continue;
        }
        const auto* exclusions = WaterSurfaceExclusions(*water, WaterSurfaceReadView::LIVE_QUERY);
        if (exclusions && (!exclusions->valid || !exclusions->boxes.empty())) {
            continue;
        }
        // PCG の岸辺の余白と揃える。静水面を基準にすることで、波が動いても植生の配置は変わらない。
        const float dryHeight = owner->transform.worldPosition.y + 0.15f;
        if (terrainWaterClip.y == 0.0f || dryHeight > terrainWaterClip.x) {
            terrainWaterClip = {dryHeight, 1.0f, 0.0f, 0.0f};
        }
    }
    return terrainWaterClip;
}

void ExtractRenderTerrains(RenderPassContext& context, renderer::RenderScene& output)
{
    using namespace terrain_extraction_detail;

    auto& scene = context.scene;
    auto& resources = context.resources;
    auto& cache = AcquireTerrainRenderCache(scene, resources);
    ReleaseRemovedTerrainResources(cache, scene, resources);
    const TerrainGridLookup gridLookup(FindActiveTerrainGrid(scene));

    // 天候はシーンに 1 つ。TerrainGrid では地形が何十個も回るのでループの外で引く。
    const ActiveWeather weather = FindActiveWeather(scene);
    const auto drawMode = context.settings.geometryDrawMode;
    const bool useGpuGeometry = context.renderer.SupportsIndirectDraw()
        && (drawMode == renderer::GeometryDrawMode::GPU_INDIRECT || drawMode == renderer::GeometryDrawMode::MESH_SHADER);

    detail::RenderCandidateLease candidateLease;
    auto& candidates = candidateLease.Entities();
    const auto entities = scene.GetEntities<TerrainComponent>();
    candidates.assign(entities.begin(), entities.end());
    const RenderSelectionLookup selection(context.settings.selectedObjects);
    bool collectPhases = false;
    if (profiler::Profiler::IsEnabled()) {
        const auto filter = profiler::Profiler::GetRequestedCaptureFilter();
        collectPhases = filter.scope == "RenderSceneExtractor::Terrains"
            && (filter.category.empty() || filter.category == "General");
    }
    std::optional<math::Vector4> grassWaterClip;
    TerrainChunkScratch scratch;
    int64_t layersNs = 0, patchesNs = 0, setupNs = 0;
    // 隣接タイルを先に失効させ、同じフレームの出力が破棄済みバッファを参照しないようにする。
    for (const EntityID entity : candidates) {
        detail::RenderPhaseTimer<> setupTimer(setupNs, collectPhases);
        RefreshTerrainGeometry(cache, scene, gridLookup, entity, resources);
    }
    for (const EntityID entity : candidates) {
        detail::RenderPhaseTimer<> setupTimer(setupNs, collectPhases);
        auto* ownerObject = scene.GetGameObject(entity);
        auto* terrainComponent = scene.GetComponent<TerrainComponent>(entity);
        if (!ownerObject || !terrainComponent) {
            continue;
        }
        auto& terrain = *terrainComponent;
        const auto& transform = ownerObject->transform;
        if (!terrain.enabled || terrain.heightData.empty()) {
            continue;
        }

        assert(terrain.heightData.size() == static_cast<size_t>(terrain.columns)
                                          * static_cast<size_t>(terrain.rows));
        assert(terrain.chunkSize > 0);

        if (!ownerObject->activeInHierarchy()) {
            continue;
        }

        renderer::RenderTerrainInput input;
        input.farLodDistance = ResolveTerrainFarLodDistance(terrain);
        const size_t lodCount = input.LODCount();
        const TerrainNeighbors neighbors = ResolveTerrainNeighbors(scene, gridLookup, entity);

        TerrainGpuState& gpu = cache.entities[entity.index];
        if (gpu.lodCount != lodCount) {
            EraseTerrainChunkCache(gpu, resources);
            gpu.lodCount = lodCount;
        }
        UpdateSplatTextures(cache, gpu, terrain, context.renderer, resources);
        setupTimer.Finish();
        detail::RenderPhaseTimer<> layersTimer(layersNs, collectPhases);

        const auto layerBinding = PrepareTerrainLayers(cache, gpu, terrain, resources);
        layersTimer.Finish();
        detail::RenderPhaseTimer<> patchesTimer(patchesNs, collectPhases);
        // 添字が無いと ResourceDescriptorHeap の範囲外を読む。描かない方が原因を切り分けやすい。
        if (layerBinding.bufferIndex == renderer::INVALID_BINDLESS_INDEX) {
            continue;
        }

        const math::Matrix4 terrainToWorld = transform.GetWorldMatrix();
        TerrainObjectCB constants{};
        constants.worldMatrix = terrainToWorld;
        constants.worldInvTranspose = math::Matrix4::InverseTransposeAffine(terrainToWorld);
        constants.wvpMatrix   = terrainToWorld;
        constants.weather = { weather.wetness, weather.darkening, weather.puddleAmount,
            ResolveTerrainFarLodDistance(terrain) };
        constants.terrainParams = {
            static_cast<float>(terrain.columns - 1) * terrain.cellSize,
            static_cast<float>(terrain.rows - 1) * terrain.cellSize,
            terrain.heightBlendDepth,
            layerBinding.usesAutoBlend ? 1.0f : 0.0f };
        constants.layerBufferIndex = layerBinding.bufferIndex;
        constants.layerCount       = layerBinding.layerCount;
        constants.splatColumns     = terrain.HasValidSplat() ? static_cast<uint32_t>(terrain.columns) : 1u;
        constants.splatRows        = terrain.HasValidSplat() ? static_cast<uint32_t>(terrain.rows) : 1u;

        input.layer = static_cast<uint32_t>(scene.GetGameObject(entity)->layer);
        input.constants = constants;
        input.splatIndices = gpu.splatIndices;
        input.splatWeights = gpu.splatWeights;
        input.chunkWorldSize = static_cast<float>(terrain.chunkSize) * terrain.cellSize;
        ApplyTerrainFiberSettings(scene, entity, layerBinding.layerCount, grassWaterClip, input);
        input.selected = selection.Contains(entity);
        PrepareTerrainPatches(gpu, terrain, neighbors, input, scratch, context);
        if (useGpuGeometry) {
            PrepareTerrainGpuGeometry(gpu, terrain, neighbors, input, cache, context.renderer, resources);
        }
        if ((context.directionalShadowBakeEnabled || context.staticShadowCache.cascadeMask != 0) && !input.patches.empty()) {
            if (!gpu.shadowBakeGeometryHash) {
                gpu.shadowBakeGeometryHash = TerrainShadowBakeGeometryHash(terrain, input.patches);
            }
            input.shadowBakeGeometryHash = gpu.shadowBakeGeometryHash;
            input.shadowBakeStatic = input.shadowBakeGeometryHash != 0;
        }
        ExtractTerrainSurface(cache, terrain, gpu, input, resources, output);
        output.terrains.push_back(std::move(input));
    }
    if (collectPhases) {
        profiler::Profiler::PushSample({"RenderSceneExtractor::Terrains::Setup", "General"}, static_cast<double>(setupNs) / 1000000.0);
        profiler::Profiler::PushSample({"RenderSceneExtractor::Terrains::Layers", "General"}, static_cast<double>(layersNs) / 1000000.0);
        profiler::Profiler::PushSample({"RenderSceneExtractor::Terrains::Patches", "General"}, static_cast<double>(patchesNs) / 1000000.0);
    }
}

}
