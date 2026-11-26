/// @file    RenderFiberExtractor.cpp
/// @brief   繊維を生やす面・材質・環境から描画入力を発行する。
/// @author  Hasegawa Jin
/// @date    2026-09-17

#include "RenderFiberExtraction.hpp"
#include <Engine/Rendering/Extraction/Effects/RenderFiberExtractor.hpp>
#include <Engine/Rendering/Passes/RenderPassContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/VFX/FiberComponent.hpp>
#include <Engine/Scene/Components/Geometry/MeshRenderer.hpp>
#include <Engine/Core/Timing/Time.hpp>
#include <Engine/Asset/Fluid/Rendering/VelocityFieldAtlas.hpp>
#include <Engine/Rendering/Extraction/Terrain/RenderTerrainExtractor.hpp>
#include <Graphics/Scene/RenderScene.hpp>
#include "../../Passes/Debug/SelectionPasses.hpp"
#include <algorithm>
#include <cmath>
#include <utility>

namespace fbzz::scene {

namespace {

using namespace fiber_extraction_detail;

FiberFrameCB BuildFiberFrame(const AmbientWind& wind)
{
    // 風・時刻はパス内で共通。物体・面ごとに検証し直さない。
    FiberFrameCB frameBase;
    frameBase.m_time = std::isfinite(Time::time) ? Time::time : 0.0f;
    if (wind.active && std::isfinite(wind.direction.x) && std::isfinite(wind.direction.y)
        && std::isfinite(wind.direction.z) && std::isfinite(wind.speed)) {
        frameBase.m_wind = wind.direction * wind.speed;
        if (!std::isfinite(frameBase.m_wind.x) || !std::isfinite(frameBase.m_wind.y) || !std::isfinite(frameBase.m_wind.z)) {
            frameBase.m_wind = {};
        }
        frameBase.m_turbulence = std::isfinite(wind.turbulence) ? wind.turbulence : 0.0f;
        frameBase.m_pulseFrequency = std::isfinite(wind.pulseFrequency) ? wind.pulseFrequency : 0.0f;
    }

    return frameBase;
}

void AppendFiberPlacementPages(GameObject& owner, FiberComponent& fiber, const renderer::Mesh& mesh,
    float fiberPadding, const renderer::RenderFiberInput& input, const FiberFlowCache& flows, FiberContactCache& contacts,
    FiberResources& cache, RenderPassContext& context, renderer::RenderScene& output)
{
    auto& resources = context.resources;
    auto& placementResources = cache.m_placements[{context.scene.GetRenderSceneGeneration(),owner.instanceId}];
    UpdateFiberPlacementResources(placementResources,mesh,fiber,input.object.world,resources);
    if (placementResources.valid) {
        const float maxScale = std::max({std::abs(owner.transform.worldScale.x),std::abs(owner.transform.worldScale.y),std::abs(owner.transform.worldScale.z)});
        for (const auto& [pageKey,placementPage] : placementResources.pages) {
            auto chunkInput = input;
            chunkInput.identity = placementPage.identity;
            chunkInput.blades.m_blades = placementPage.buffer; chunkInput.blades.m_bladeCount = placementPage.count;
            const auto center = input.object.world*math::Vector4{placementPage.center.x,placementPage.center.y,placementPage.center.z,1};
            chunkInput.hasBounds = true;
            chunkInput.boundsCenter = {center.x,center.y,center.z};
            chunkInput.lodCenter = chunkInput.boundsCenter;
            chunkInput.boundsRadius = placementPage.radius*maxScale+fiberPadding+fiber.m_bladeWidth;
            chunkInput.frame.m_flowCount = CountFiberFlows(flows.m_current,chunkInput.frame.m_flowChannels,chunkInput.boundsCenter,chunkInput.boundsRadius);
            chunkInput.frame.m_flowPreviousCount = CountFiberFlows(flows.m_previous,chunkInput.frame.m_flowChannels,chunkInput.boundsCenter,chunkInput.boundsRadius);
            chunkInput.contacts = SelectFiberContacts(contacts, chunkInput);
            output.fibers.push_back(std::move(chunkInput));
        }
    }
}

} // namespace

void ExtractRenderFibers(RenderPassContext& context, renderer::RenderScene& output)
{
    if (context.scene.GetEntities<FiberComponent>().empty()) {
        return;
    }
    const RenderSelectionLookup selection(context.settings.selectedObjects);
    auto& resources = context.resources;
    auto& cache = GetFiberResources(resources);
    const auto wind = context.scene.FlowFrame().ambient;
    auto& contacts=cache.m_contactStates[context.scene.GetRenderSceneGeneration()];
    UpdateFiberContacts(contacts,context.scene);

    const auto& flows = UpdateFiberFlows(cache, context.scene, resources);
    const auto terrainWaterClip = GetTerrainGrassWaterClip(context.scene);
    const auto frameBase = BuildFiberFrame(wind);

    static std::vector<FiberSurface> surfaces;
    static uint64_t nextIdentity = 0;
    for (auto& owner : context.scene.GameObjects()) {
        if (!HasFiberSurface(owner, ~0u)) {
            continue;
        }
        auto& fiber = *owner.GetComponent<FiberComponent>();
        const auto& materialEntry = GetFiberMaterial(cache, resources, fiber.m_materialPath);
        if (!materialEntry.m_valid) {
            continue;
        }
        const asset::FiberMaterialSettings settings = ApplyFiberOverrides(materialEntry.m_settings, fiber, cache, resources);
        if (settings.m_length <= 0.0f || settings.m_density <= 0.0f) {
            continue;
        }
        // 毛丈と曲げを含めたカリング球の余白。静的メッシュとスキンで同じ値を使う。
        const float fiberPadding = context.cullingBoundsPadding + settings.m_length * 3.0f + settings.m_maxBend;
        CollectFiberSurfaces(owner, fiber, fiberPadding, cache, context, surfaces);
        if (!fiber.m_renderIdentity) {
            fiber.m_renderIdentity = ++nextIdentity;
        }
        const bool selected = selection.Contains(owner.GetID());
        for (const auto& surface : surfaces) {
            const auto* mesh = surface.mesh;
            if (!surface.vertices.IsValid() || !mesh->indexBuffer.IsValid()) {
                continue;
            }
            renderer::RenderFiberInput input;
            input.identity = fiber.m_renderIdentity;
            input.layer = static_cast<uint32_t>(owner.layer);
            input.selected = selected;
            input.skinned = mesh->isSkinned;
            input.preparedSkinning = surface.preparedSkinning;
            input.preparedSkinningWrite = surface.preparedSkinningWrite;
            input.cast = fiber.m_castShadows && surface.cast;
            input.validPreviousSkin = surface.validPreviousSkin;
            input.object.world = owner.transform.GetWorldMatrix();
            input.object.worldInvTranspose = math::Matrix4::InverseTransposeAffine(input.object.world);
            input.object.objectParams.x = surface.dither;
            input.hasBounds = input.skinned ? surface.hasSkinBounds : mesh->boundsRadius > 0;
            const auto bounds = input.skinned ? surface.skinBounds : ComputeWorldBounds(owner.transform, *mesh, fiberPadding);
            input.boundsCenter = input.hasBounds ? bounds.center : owner.transform.worldPosition;
            input.boundsRadius = input.hasBounds ? bounds.radius : -1;
            const auto center = input.object.world * math::Vector4{mesh->boundsCenter.x, mesh->boundsCenter.y, mesh->boundsCenter.z, 1};
            input.lodCenter = input.skinned && surface.hasSkinBounds ? bounds.center : math::Vector3{center.x,center.y,center.z};
            input.settings.m_mode = fiber.m_mode;
            input.settings.m_shellCount = fiber.m_shellCount;
            input.settings.m_minShellCount = fiber.m_minShellCount;
            input.settings.m_shadowShellCount = fiber.m_shadowShellCount;
            input.settings.m_bladeSegments = fiber.m_bladeSegments;
            input.settings.m_shadowBladeSegments = fiber.m_shadowBladeSegments;
            input.settings.m_distanceLod = fiber.m_distanceLod;
            input.settings.m_lodNear = fiber.m_lodNear;
            input.settings.m_lodFar = fiber.m_lodFar;
            const float sparseStart = fiber.m_terrainSparseLodStart;
            float bladeDensity = fiber.m_bladeDensity;
            if (surface.terrain && fiber.m_mode == FiberRenderMode::BLADE && fiber.m_distanceLod
                && sparseStart > std::max(fiber.m_lodNear, 0.0f) && sparseStart < fiber.m_lodFar) {
                input.settings.m_terrainSparseLodStart = sparseStart;
                if ((input.lodCenter - context.camera.m_position).Length() >= sparseStart) {
                    bladeDensity *= renderer::TERRAIN_SPARSE_BLADE_DENSITY_SCALE;
                }
            }
            input.material = settings;
            input.appearance = FiberAppearanceFor(materialEntry.m_appearance, fiber.m_mode);
            input.frame = frameBase;
            if (surface.terrain) {
                input.frame.m_terrainWaterClip = terrainWaterClip;
            }
            input.frame.m_flowChannels = static_cast<uint32_t>(fiber.m_flowChannels);
            input.frame.m_flowCount = CountFiberFlows(flows.m_current, input.frame.m_flowChannels, input.boundsCenter, input.boundsRadius);
            input.frame.m_flowPreviousFirst = static_cast<uint32_t>(flows.m_currentPacked.size());
            input.frame.m_flowPreviousCount = CountFiberFlows(flows.m_previous, input.frame.m_flowChannels, input.boundsCenter, input.boundsRadius);
            input.vertices = surface.vertices; input.indices = mesh->indexBuffer; input.indexCount = mesh->indexCount;
            input.skinnedVertices = surface.skinnedVertices; input.skin = surface.skin; input.previousSkin = surface.previousSkin;
            input.flows = flows.m_buffer;
            input.velocityField = asset::VelocityFieldAtlas::Texture(resources);
            const auto resolveShape = [&](bool blades) {
                const auto& buffers = GetFiberFinBuffers(cache, resources, *mesh, fiber, bladeDensity, blades,
                    surface.densityFromAlpha, surface.terrain);
                return renderer::FiberResolvedGeometry{buffers.m_vertices, buffers.m_indices, buffers.m_indexCount, buffers.m_blades, buffers.m_bladeCount};
            };
            if (fiber.m_mode == FiberRenderMode::FIN || fiber.m_mode == FiberRenderMode::HYBRID) {
                input.fins = resolveShape(false);
            }
            if (fiber.m_mode == FiberRenderMode::BLADE && !input.skinned && fiber.m_chunkedPlacement
                && owner.GetComponent<MeshRenderer>()) {
                AppendFiberPlacementPages(owner, fiber, *mesh, fiberPadding, input, flows, contacts, cache, context, output);
                continue;
            }
            if (fiber.m_mode == FiberRenderMode::BLADE && !input.skinned) {
                input.blades = resolveShape(true);
            }
            input.contacts = SelectFiberContacts(contacts, input);
            output.fibers.push_back(std::move(input));
        }
    }
    surfaces.clear();
    ExtractRayFiberSources(context.scene, resources, output, context.experimentalRayTracingEnabled);
}

} // namespace fbzz::scene
