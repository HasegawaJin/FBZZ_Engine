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

    const bool needsLdrIntermediate = rs.fxaaEnabled || ctx.selectionOutlineEnabled;
    r.SetRenderTarget(needsLdrIntermediate ? h.ldrRT : ctx.outputRT, resources);

    PostProcCB postData{};
    postData.texelSize[0] = 1.0f / static_cast<float>(ctx.width);
    postData.texelSize[1] = 1.0f / static_cast<float>(ctx.height);
    postData.screenSize[0] = static_cast<float>(ctx.width);
    postData.screenSize[1] = static_cast<float>(ctx.height);
    postData.exposure = rs.exposure;
    postData.fogDensity = rs.fogEnabled ? rs.fogDensity : 0.0f;
    postData.fogFar = rs.fogFar;
    postData.fogColor[0] = rs.fogColor[0];
    postData.fogColor[1] = rs.fogColor[1];
    postData.fogColor[2] = rs.fogColor[2];
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
    compositeDC.textures[10] = rs.bloomEnabled ? h.bloomFull : renderer::ResourceHandle<renderer::TextureTag>{};
    r.Submit(compositeDC, resources);

    h.fxaaInput = resources.GetColorTexture(h.ldrRT, 0);
}

} // namespace fbzz::scene
