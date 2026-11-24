/// @file    RenderWaterExtractor.cpp
/// @brief   シーンの水域を描画パスに渡す抽出処理。
/// @author  Hasegawa Jin
/// @date    2026-06-18

#include "../RenderCandidateLease.hpp"
#include "RenderWaterAppearance.hpp"
#include "RenderWaterResources.hpp"
#include <Core/Profiler/ProfileScope.hpp>
#include <Engine/Asset/Fluid/Rendering/VelocityFieldAtlas.hpp>
#include <Engine/Core/Timing/Time.hpp>
#include <Engine/Scene/Components/Water/WaterComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Rendering/Passes/RenderPassContext.hpp>
#include <Engine/Rendering/Extraction/Water/RenderWaterExtractor.hpp>
#include <Engine/Scene/Systems/Water/WaterSystem.hpp>
#include <Engine/Scene/Water/Surface/WaterDepthField.hpp>
#include <Engine/Scene/Water/Surface/WaterExclusion.hpp>
#include <Engine/Scene/Water/Surface/WaterSurfaceQuery.hpp>
#include <Graphics/Passes/Geometry/Surfaces/WaterRenderPass.hpp>
#include <Graphics/Scene/RenderScene.hpp>
#include <algorithm>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fbzz::scene {

void ExtractRenderWater(RenderPassContext& context, renderer::RenderScene& output)
{
    using namespace water_render_detail;

    auto& scene = context.scene;
    auto& resources = context.resources;
    for (auto [water, transform] : scene.View<WaterComponent, Transform>()) {
        if (!water.enabled || WaterSurfaceEffectiveShape(water, WaterSurfaceReadView::SIMULATION)
                != WaterSurfaceShape::RECTANGLE
            || !IsWaterSurfaceSupported(water, transform, WaterSurfaceReadView::SIMULATION)) continue;
        water.extentX = (std::clamp)(water.extentX, 0.1f, 10000.0f);
        water.extentZ = (std::clamp)(water.extentZ, 0.1f, 10000.0f);
    }
    {
        FBZZ_PROFILE_SCOPE("WaterPrepare::Surfaces");
        PrepareWaterSceneSurfaces(scene);
    }
    auto& cache = WaterRenderCache::Acquire(scene, resources);
    cache.ReleaseUnavailableBodies(scene, resources);
    {
        FBZZ_PROFILE_SCOPE("WaterPrepare::DepthFields");
        PrepareWaterDepthFields(scene, WaterSurfaceReadView::SIMULATION);
    }
    WaterTerrainInfoInputs terrainInputs;
    const bool validTerrainInputs = cache.CollectTerrainInputs(scene, terrainInputs);
    if (!validTerrainInputs) terrainInputs = {};
    std::unordered_set<uint64_t> selectedZoneOwners;
    for (const auto& selected : context.settings.selectedObjects) {
        const EntityID selectedId{selected.index, selected.generation};
        const WaterComponent* selectedWater = scene.GetComponent<WaterComponent>(selectedId);
        if (selectedWater) {
            const EntityID owner = WaterSurfaceZoneOwner(*selectedWater, WaterSurfaceReadView::SIMULATION);
            if (owner.IsValid())
                selectedZoneOwners.insert((static_cast<uint64_t>(owner.generation) << 32u) | owner.index);
        }
    }
    std::vector<HorizonMaterialLink> horizonMaterialLinks;
    detail::RenderCandidateLease candidateLease;
    auto& candidates = candidateLease.Entities();
    const auto entities = scene.GetEntities<WaterComponent>();
    candidates.assign(entities.begin(), entities.end());
    for (const EntityID entity : candidates) {
        auto* ownerObject = scene.GetGameObject(entity);
        auto* waterComponent = scene.GetComponent<WaterComponent>(entity);
        if (!ownerObject || !waterComponent) continue;
        auto& water = *waterComponent;
        const auto& objectTransform = ownerObject->transform;
        if (!water.enabled || WaterSurfaceZoneOwner(water, WaterSurfaceReadView::SIMULATION).IsValid()) continue;
        const Transform& surfaceTransform = WaterSurfaceTransform(water, objectTransform,
            WaterSurfaceReadView::SIMULATION);
        if (!IsWaterSurfaceSupported(water, surfaceTransform, WaterSurfaceReadView::SIMULATION)) continue;
        /// @note 無効な除外設定を空のmaskへ置き換えると乾燥領域が水没するため、描画はfail-closed。
        if (const auto* exclusions = WaterSurfaceExclusions(water, WaterSurfaceReadView::SIMULATION);
            exclusions && !exclusions->valid) continue;
        const bool splineSurface = UsesWaterSurfaceGeometry(water, WaterSurfaceReadView::SIMULATION);
        const bool horizonSurface = WaterSurfaceUsesHorizon(water, WaterSurfaceReadView::SIMULATION);
        const WaterSplineSnapshot* spline = WaterSurfaceGeometry(water, WaterSurfaceReadView::SIMULATION);
        if (splineSurface && !spline) continue;
        NormalizeWaterMeshSettings(water, splineSurface);

        const WaterCacheKey cacheKey{scene.GetRenderSceneGeneration(), entity.index, entity.generation};
        WaterRenderCache::Invalidate(cacheKey, water.meshDirty, water.texDirty || water.foamDirty);
        if (!ownerObject->activeInHierarchy()) continue;

        const auto margin = ComputeWaveMargin(water, horizonSurface);
        const auto materialSnapshot = LoadWaterMaterial(water.materialPath);
        const auto* material = materialSnapshot ? &materialSnapshot->Data() : nullptr;
        const auto textureRequest = ResolveWaterTextureRequest(water, surfaceTransform, terrainInputs,
            validTerrainInputs, material);

        const bool buildSurfaceClusters = splineSurface
            && context.settings.geometryDrawMode == renderer::GeometryDrawMode::MESH_SHADER;
        cache.PrepareMesh(cacheKey, water, resources, buildSurfaceClusters);
        cache.PrepareTerrainTextures(cacheKey, water, surfaceTransform, terrainInputs, textureRequest,
            context.renderer, resources);
        const auto& rippleState = cache.PrepareRipples(cacheKey, water, surfaceTransform, resources);

        WaterMesh& mesh = cache.MeshFor(cacheKey);
        const bool stripsReady = mesh.patchGrid ? mesh.patchGrid->stripsReady : mesh.stripsReady;
        if (!splineSurface && !stripsReady
            && context.settings.rasterGeometryMode == renderer::RasterGeometryMode::TRIANGLE_STRIP
            && context.settings.geometryDrawMode != renderer::GeometryDrawMode::GPU_INDIRECT) {
            FBZZ_PROFILE_SCOPE("WaterPrepare::StripIndices");
            PrepareWaterStripBuffers(water, mesh, resources);
        }
        renderer::RenderWaterInput input;
        input.layer = static_cast<uint32_t>(scene.GetGameObject(entity)->layer);
        const uint64_t entityKey = (static_cast<uint64_t>(entity.generation) << 32u) | entity.index;
        input.selected = context.settings.IsSelected(entity.index, entity.generation)
            || selectedZoneOwners.contains(entityKey);
        input.margin = margin;
        PopulateWaterGeometry(input, water, mesh, splineSurface, horizonSurface, buildSurfaceClusters);
        const auto& detailNoise = renderer::GetWaterDetailNoise(resources);
        {
            FBZZ_PROFILE_SCOPE("WaterPrepare::Constants");
            input.constants = BuildWaterConstants(water, material, surfaceTransform,
                math::Matrix4::Identity(), Time::time, 1.0f, detailNoise, resources);
            input.constants.waterfallSeams = water.EffectiveWaterfallSeams(WaterSurfaceReadView::SIMULATION);
            input.constants.horizonWaveSeams = water.EffectiveHorizonWaveSeams(WaterSurfaceReadView::SIMULATION);
        }
        if (splineSurface) {
            ApplySplineWaterConstants(water, *spline, input.constants);
            if (input.constants.horizonWaveSeams.front().w > 0.0f && spline->horizonMouth.w > 0.0f) {
                const auto* receiver = water.horizonMaterialReceiver.Resolve(scene);
                if (receiver) {
                    const auto receiverId = receiver->GetID();
                    horizonMaterialLinks.push_back({output.water.size(),
                        (static_cast<uint64_t>(receiverId.generation) << 32u) | receiverId.index, spline->horizonMouth});
                }
            }
        }
        input.constants.terrainInfoParams = cache.TerrainTexturesFor(cacheKey).terrainInfoParams;
        input.constants.terrainDomain = cache.TerrainTexturesFor(cacheKey).terrainDomain;
        {
            FBZZ_PROFILE_SCOPE("WaterPrepare::Medium");
            input.medium = BuildMediumInput(water, surfaceTransform, entity, material, mesh, margin, input.constants);
        }
        input.medium.depthIsUniform = cache.TerrainTexturesFor(cacheKey).depthIsUniform;
        input.effects = BuildWaterEffectParams(material);
        ResolveZoneWaterMaterials(scene, entity, water, surfaceTransform, input, detailNoise, resources);
        input.detailNoise = detailNoise.texture;
        input.foam = cache.TerrainTexturesFor(cacheKey).terrainInfoTexture;
        input.ripple = rippleState.texture;
        input.velocityField = asset::VelocityFieldAtlas::Texture(resources);
        BindWaterAppearance(material, cache, cacheKey, input, resources);
        output.water.push_back(std::move(input));
    }
    LinkHorizonWaterMaterials(output.water, horizonMaterialLinks);
}

}
