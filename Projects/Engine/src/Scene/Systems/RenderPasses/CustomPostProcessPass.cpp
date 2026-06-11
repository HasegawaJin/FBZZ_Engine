// FBZZ Engine
// CustomPostProcessPass.cpp | fbzz::scene
// User shader post-process render pass implementation
#include "PostProcessPasses.hpp"
#include <Engine/Scene/Systems/RenderPassContext.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Renderer/SamplerMode.hpp>

namespace fbzz::scene {

void ExecuteCustomPostProcessPass(RenderPassContext& ctx, uint32_t customIndex, uint32_t outputIndex)
{
    auto& h = ctx.handles;
    const auto& pp = ctx.settings.postProcess;

    if (customIndex >= pp.customEffects.size() || customIndex >= h.customPostProcessShaders.size())
        return;

    const auto& custom = pp.customEffects[customIndex];
    const auto shader = h.customPostProcessShaders[customIndex];
    if (!custom.enabled || !shader.IsValid())
        return;

    const bool needsIntermediate = outputIndex < 2;
    if (needsIntermediate && !h.customPostProcessRT[outputIndex].IsValid())
        return;

    auto& r = ctx.renderer;
    auto& resources = ctx.resources;
    r.SetRenderTarget(needsIntermediate ? h.customPostProcessRT[outputIndex] : ctx.outputRT, resources);
    r.SetSampler(0, renderer::SamplerMode::CLAMP_LINEAR);

    PostProcCB postData{};
    postData.texelSize[0] = 1.0f / static_cast<float>(ctx.width);
    postData.texelSize[1] = 1.0f / static_cast<float>(ctx.height);
    postData.screenSize[0] = static_cast<float>(ctx.width);
    postData.screenSize[1] = static_cast<float>(ctx.height);
    postData.time = Time::time;
    postData.customIntensity = custom.intensity;
    postData.customBlend = custom.blend;
    postData.customParameters[0] = custom.parameters[0];
    postData.customParameters[1] = custom.parameters[1];
    postData.customParameters[2] = custom.parameters[2];
    postData.customParameters[3] = custom.parameters[3];
    resources.Update(h.postprocCB, &postData, sizeof(PostProcCB));

    renderer::DrawCall customDC;
    customDC.shader = shader;
    customDC.pipelineState = h.postprocPSO;
    customDC.vertexCount = 3;
    customDC.constantBuffers[5] = h.postprocCB;
    customDC.textures[5] = h.postProcessInput.IsValid()
        ? h.postProcessInput
        : resources.GetColorTexture(h.ldrRT, 0);
    r.Submit(customDC, resources);

    if (needsIntermediate) {
        h.postProcessInput = resources.GetColorTexture(h.customPostProcessRT[outputIndex], 0);
        h.fxaaInput = h.postProcessInput;
    }
}

} // namespace fbzz::scene
