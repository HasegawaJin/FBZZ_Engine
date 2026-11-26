/// @file    ViewRenderPlan.cpp
/// @brief   現在のビュー資源から実行可能な描画構成を解決する。
/// @author  Hasegawa Jin
/// @date    2026-09-30
#include <Graphics/Pipeline/View/ViewPreparation.hpp>
#include <Graphics/Renderer/IRenderer.hpp>

namespace fbzz::renderer {

ResolvedRenderPlan PrepareViewRenderPlan(ResourceManager& resources, IRenderer& renderer,
    const RenderSettings& settings, RenderViewResources& view,
    const RenderSharedResources& shared, const RenderPassHandles& handles,
    bool experimentalRayTracingEnabled)
{
    const auto hasViewExtent = [&](ResourceHandle<RenderTargetTag> handle) {
        const auto* target = resources.Get(handle);
        return target && target->GetWidth() == view.width && target->GetHeight() == view.height;
    };
    const auto* output = resources.Get(view.output);
    RenderAvailability available;
    available.experimentalRayTracingEnabled = experimentalRayTracingEnabled;
    available.outputsReady = view.nativeWidth > 0 && view.nativeHeight > 0
        && (view.output.IsValid()
            ? output && output->GetWidth() == view.nativeWidth && output->GetHeight() == view.nativeHeight
            : renderer.GetWidth() == view.nativeWidth && renderer.GetHeight() == view.nativeHeight);

    available.opaque = {
        hasViewExtent(view.gbuffer) && resources.Get(view.gbuffer)->GetColorCount() == GBUFFER_COLOR_COUNT
            && resources.Get(shared.gbufferShader),
        resources.Get(shared.deferredLightingShader) != nullptr,
        resources.Get(shared.depthCopyShader) != nullptr,
    };
    available.rasterPipelineReady = hasViewExtent(view.hdr) && hasViewExtent(view.ldr)
        && resources.Get(shared.frameCB) && resources.Get(shared.objectCB)
        && resources.Get(shared.lightCB) && resources.Get(shared.postprocCB) && resources.Get(view.advancedGraphicsCB)
        && resources.Get(settings.IsWireframe() ? shared.wireframePSO : shared.defaultPSO)
        && resources.Get(shared.postprocPSO) && resources.Get(shared.compositeShader);
    if (view.needsUpscale && available.rasterPipelineReady) {
        const bool magnify = view.width < view.nativeWidth || view.height < view.nativeHeight;
        const auto filter = magnify ? shared.upscaleShader : shared.downscaleShader;
        available.rasterPipelineReady = hasViewExtent(view.upscaleSrc)
            && (resources.Get(filter) || resources.Get(shared.copyColorShader));
    }
    available.clusteredLightingReady = !settings.IsUnlit() && !settings.clustered.forceAllLights
        && resources.Get(handles.punctualLightBuffer) && resources.Get(shared.clusterCB)
        && resources.Get(shared.clusterIndexBuffer) && resources.Get(shared.clusterCullCS);

    SceneRayCoverage coverage;
    coverage.reflection = view.rayReflectionCovered;
    coverage.path = view.rayPathCovered;

    const auto& reflection = view.rayReflection;
    const auto& path = view.rayPath;
    available.raySceneReady = reflection.gpu.ready || path.gpu.ready;
    available.reflectionPipelineReady = reflection.gpu.ready
        && resources.Get(reflection.output) && resources.Get(reflection.constants)
        && resources.Get(shared.rayReflectionShader) && handles.rayReflectionResult == reflection.output;
    available.pathPipelineReady = view.rayPathPrepared && path.gpu.ready && hasViewExtent(view.hdr)
        && resources.Get(path.output) && resources.Get(path.firstSurface)
        && resources.Get(path.firstMaterial) && resources.Get(path.firstGeometry)
        && resources.Get(path.historyBuffer) && resources.Get(path.idsBuffer) && resources.Get(path.constants)
        && resources.Get(shared.rayPathShader) && resources.Get(shared.rayPathResolveShader)
        && resources.Get(shared.rayPathResolvePSO) && resources.Get(shared.copyColorShader) && available.rasterPipelineReady;
    const bool gameSurfaceReady = path.game.prepared && available.opaque.depthNormal;
    if (settings.modeRequest.pathProfile == PathTracingProfile::GAME) {
        available.pathPipelineReady = available.pathPipelineReady && gameSurfaceReady
            && resources.Get(shared.rayGameReconstructionShader);
    }
    available.rasterSurfaceReady = view.rayPathPrepared && gameSurfaceReady;

    view.renderPlan = ResolveRenderPlan(settings, settings.modeRequest, renderer.GetCapabilities(), coverage, available);
    return view.renderPlan;
}

} // namespace fbzz::renderer
