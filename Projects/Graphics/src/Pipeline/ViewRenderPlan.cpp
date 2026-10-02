/// @file    ViewRenderPlan.cpp
/// @brief   現在のビュー資源から実行可能な描画構成を解決する。
/// @author  Hasegawa Jin
/// @date    2026-09-30
#include <Graphics/Pipeline/ViewPreparation.hpp>
#include <Graphics/Renderer/IRenderer.hpp>

namespace fbzz::renderer {

ResolvedRenderPlan PrepareViewRenderPlan(ResourceManager& resources, IRenderer& renderer,
    const RenderSettings& settings, RenderViewResources& view,
    const RenderSharedResources& shared, const RenderPassHandles& handles)
{
    const auto targetReady = [&](ResourceHandle<RenderTargetTag> handle) {
        const auto* target = resources.Get(handle);
        return target && target->GetWidth() == view.width && target->GetHeight() == view.height;
    };
    const auto* output = resources.Get(view.output);
    RenderAvailability available;
    available.outputsReady = view.nativeWidth > 0 && view.nativeHeight > 0
        && (view.output.IsValid()
            ? output && output->GetWidth() == view.nativeWidth && output->GetHeight() == view.nativeHeight
            : renderer.GetWidth() == view.nativeWidth && renderer.GetHeight() == view.nativeHeight);
    available.opaque = {
        targetReady(view.gbuffer) && resources.Get(view.gbuffer)->GetColorCount() == GBUFFER_COLOR_COUNT
            && resources.Get(shared.gbufferShader),
        resources.Get(shared.deferredLightingShader) != nullptr,
        resources.Get(shared.depthCopyShader) != nullptr,
    };
    available.rasterPipelineReady = targetReady(view.hdr) && targetReady(view.ldr)
        && resources.Get(shared.frameCB) && resources.Get(shared.objectCB)
        && resources.Get(shared.lightCB) && resources.Get(shared.postprocCB)
        && resources.Get(view.advancedGraphicsCB)
        && resources.Get(settings.IsWireframe() ? shared.wireframePSO : shared.defaultPSO)
        && resources.Get(shared.postprocPSO) && resources.Get(shared.compositeShader);
    if (view.needsUpscale) {
        const bool magnify = view.width < view.nativeWidth || view.height < view.nativeHeight;
        const auto filter = magnify ? shared.upscaleShader : shared.downscaleShader;
        available.rasterPipelineReady = available.rasterPipelineReady
            && targetReady(view.upscaleSrc)
            && (resources.Get(filter) || resources.Get(shared.copyColorShader));
    }
    available.clusteredLightingReady = !settings.IsUnlit() && !settings.clustered.forceAllLights
        && resources.Get(handles.punctualLightBuffer) && resources.Get(shared.clusterCB)
        && resources.Get(shared.clusterIndexBuffer) && resources.Get(shared.clusterCullCS);
    SceneRayCoverage coverage;
    coverage.reflection = view.rayReflectionCovered;
    coverage.path = view.rayPathCovered;
    available.raySceneReady = view.rayReflection.gpu.ready || view.rayPath.gpu.ready;
    available.reflectionPipelineReady = view.rayReflection.gpu.ready
        && resources.Get(view.rayReflection.output) && resources.Get(view.rayReflection.constants)
        && resources.Get(shared.rayReflectionShader) && handles.rayReflectionResult == view.rayReflection.output;
    available.pathPipelineReady = view.rayPathPrepared && view.rayPath.gpu.ready && targetReady(view.hdr)
        && resources.Get(view.rayPath.output) && resources.Get(view.rayPath.firstSurface)
        && resources.Get(view.rayPath.firstMaterial) && resources.Get(view.rayPath.firstGeometry)
        && resources.Get(view.rayPath.historyBuffer) && resources.Get(view.rayPath.idsBuffer)
        && resources.Get(view.rayPath.constants) && resources.Get(shared.rayPathShader)
        && resources.Get(shared.rayPathResolveShader) && resources.Get(shared.rayPathResolvePSO)
        && resources.Get(shared.copyColorShader) && available.rasterPipelineReady;
    if (settings.modeRequest.pathProfile == PathTracingProfile::GAME)
        available.pathPipelineReady = available.pathPipelineReady && view.rayPath.game.prepared
            && available.opaque.depthNormal && resources.Get(shared.rayGameReconstructionShader);
    available.rasterSurfaceReady = view.rayPathPrepared && view.rayPath.game.prepared && available.opaque.depthNormal;
    view.renderPlan = ResolveRenderPlan(settings, settings.modeRequest,
        renderer.GetCapabilities(), coverage, available);
    return view.renderPlan;
}

}
