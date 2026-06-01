// FBZZ Engine
// FxaaPass.cpp | fbzz::scene
// FXAA render pass implementation
#include "PostProcessPasses.hpp"
#include <Engine/Scene/Systems/RenderPassContext.hpp>
#include <Engine/Renderer/DrawCall.hpp>

namespace fbzz::scene {

void ExecuteFxaaPass(RenderPassContext& ctx)
{
    auto& h = ctx.handles;
    const auto& rs = ctx.settings;

    if (rs.postProcess.fxaaEnabled && h.fxaaShader.IsValid() && h.ldrRT.IsValid())
    {
        ctx.renderer.SetRenderTarget(ctx.outputRT, ctx.resources);

        renderer::DrawCall fxaaDC;
        fxaaDC.shader = h.fxaaShader;
        fxaaDC.pipelineState = h.postprocPSO;
        fxaaDC.vertexCount = 3;
        fxaaDC.constantBuffers[5] = h.postprocCB;
        fxaaDC.textures[5] = h.fxaaInput;
        ctx.renderer.Submit(fxaaDC, ctx.resources);
    }
}

} // namespace fbzz::scene
