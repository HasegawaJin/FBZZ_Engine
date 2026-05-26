// FBZZ Engine
// CompositePass.cpp | fbzz::scene
// Composite render pass implementation
#include "PostProcessPasses.hpp"
#include "RenderPassContext.hpp"
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Renderer/SamplerMode.hpp>

namespace fbzz::scene {

void ExecuteCompositePass(RenderPassContext& ctx)
{
    auto& r = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h = ctx.handles;
    const auto& rs = ctx.settings;

    const auto& pp = rs.postProcess;
    const bool needsLdrIntermediate = pp.fxaaEnabled || ctx.selectionOutlineEnabled;
    r.SetRenderTarget(needsLdrIntermediate ? h.ldrRT : ctx.outputRT, resources);

    PostProcCB postData{};
    postData.texelSize[0] = 1.0f / static_cast<float>(ctx.width);
    postData.texelSize[1] = 1.0f / static_cast<float>(ctx.height);
    postData.screenSize[0] = static_cast<float>(ctx.width);
    postData.screenSize[1] = static_cast<float>(ctx.height);
    postData.exposure = pp.exposure;
    postData.bloomIntensity = pp.bloom.enabled ? pp.bloom.intensity : 0.0f;
    postData.fogDensity = pp.fog.enabled ? pp.fog.density : 0.0f;
    postData.fogFar = pp.fog.farDistance;
    postData.fogColor[0] = pp.fog.color[0];
    postData.fogColor[1] = pp.fog.color[1];
    postData.fogColor[2] = pp.fog.color[2];
    postData.contrast = pp.colorGrading.enabled ? pp.colorGrading.contrast : 0.0f;
    postData.saturation = pp.colorGrading.enabled ? pp.colorGrading.saturation : 1.0f;
    postData.hueShift = pp.colorGrading.enabled ? pp.colorGrading.hueShift : 0.0f;
    postData.temperature = pp.colorGrading.enabled ? pp.colorGrading.temperature : 0.0f;
    postData.tint = pp.colorGrading.enabled ? pp.colorGrading.tint : 0.0f;
    postData.vignetteIntensity = pp.vignette.enabled ? pp.vignette.intensity : 0.0f;
    postData.vignetteSmoothness = pp.vignette.smoothness;
    postData.vignetteRoundness = pp.vignette.roundness;
    postData.vignetteColor[0] = pp.vignette.color[0];
    postData.vignetteColor[1] = pp.vignette.color[1];
    postData.vignetteColor[2] = pp.vignette.color[2];
    postData.filmGrainIntensity = pp.filmGrain.enabled ? pp.filmGrain.intensity : 0.0f;
    postData.filmGrainResponse = pp.filmGrain.response;
    postData.chromaticAberration = pp.lens.chromaticAberrationEnabled ? pp.lens.chromaticAberration : 0.0f;
    postData.lensDistortion = pp.lens.distortionEnabled ? pp.lens.distortion : 0.0f;
    resources.Update(h.postprocCB, &postData, sizeof(PostProcCB));

    r.SetSampler(0, renderer::SamplerMode::CLAMP_LINEAR);

    renderer::DrawCall compositeDC;
    compositeDC.shader = h.compositeShader;
    compositeDC.pipelineState = h.postprocPSO;
    compositeDC.vertexCount = 3;
    compositeDC.constantBuffers[0] = h.frameCB;
    compositeDC.constantBuffers[5] = h.postprocCB;
    compositeDC.textures[5] = resources.GetColorTexture(h.hdrRT, 0);
    compositeDC.textures[7] = resources.GetDepthTexture(h.hdrRT);
    compositeDC.textures[10] = pp.bloom.enabled ? h.bloomFull : renderer::ResourceHandle<renderer::TextureTag>{};
    r.Submit(compositeDC, resources);

    h.fxaaInput = resources.GetColorTexture(h.ldrRT, 0);
}

} // namespace fbzz::scene
