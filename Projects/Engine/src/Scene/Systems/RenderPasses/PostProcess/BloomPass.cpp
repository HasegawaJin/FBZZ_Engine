// FBZZ Engine
// BloomPass.cpp | fbzz::scene
// Bloom render pass implementation
#include "PostProcessPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/ComputeCall.hpp>
#include <algorithm>

namespace fbzz::scene {

void ExecuteBloomPass(RenderPassContext& ctx)
{
    auto& r = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h = ctx.handles;
    const auto& rs = ctx.settings;

    if (rs.postProcess.bloom.enabled && h.bloomDownShader.IsValid() && h.bloomUpShader.IsValid() &&
        h.bloomHalf.IsValid() && h.bloomFull.IsValid())
    {
        PostProcCB halfData{};
        halfData.texelSize[0] = 1.0f / static_cast<float>(ctx.width);
        halfData.texelSize[1] = 1.0f / static_cast<float>(ctx.height);
        halfData.screenSize[0] = static_cast<float>(ctx.width);
        halfData.screenSize[1] = static_cast<float>(ctx.height);
        halfData.bloomThreshold = rs.postProcess.bloom.threshold;
        halfData.bloomSoftKnee = rs.postProcess.bloom.softKnee;
        resources.Update(h.postprocCB, &halfData, sizeof(PostProcCB));

        renderer::ComputeCall bloomDownDC;
        bloomDownDC.shader = h.bloomDownShader;
        bloomDownDC.constantBuffers[5] = h.postprocCB;
        bloomDownDC.srvInputs[10] = resources.GetColorTexture(h.hdrRT, 0);
        bloomDownDC.uavOutputs[0] = h.bloomHalf;
        bloomDownDC.dispatchX = (ctx.width / 2 + 7) / 8;
        bloomDownDC.dispatchY = (ctx.height / 2 + 7) / 8;
        bloomDownDC.dispatchZ = 1;
        r.Dispatch(bloomDownDC, resources);

        PostProcCB fullData{};
        fullData.texelSize[0] = 1.0f / static_cast<float>(std::max(1u, ctx.width / 2));
        fullData.texelSize[1] = 1.0f / static_cast<float>(std::max(1u, ctx.height / 2));
        fullData.screenSize[0] = static_cast<float>(ctx.width);
        fullData.screenSize[1] = static_cast<float>(ctx.height);
        resources.Update(h.postprocCB, &fullData, sizeof(PostProcCB));

        renderer::ComputeCall bloomUpDC;
        bloomUpDC.shader = h.bloomUpShader;
        bloomUpDC.constantBuffers[5] = h.postprocCB;
        bloomUpDC.srvInputs[10] = h.bloomHalf;
        bloomUpDC.uavOutputs[0] = h.bloomFull;
        bloomUpDC.dispatchX = (ctx.width + 7) / 8;
        bloomUpDC.dispatchY = (ctx.height + 7) / 8;
        bloomUpDC.dispatchZ = 1;
        r.Dispatch(bloomUpDC, resources);
    }
}

} // namespace fbzz::scene
