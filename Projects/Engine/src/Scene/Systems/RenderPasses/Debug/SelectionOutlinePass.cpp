// FBZZ Engine
// SelectionOutlinePass.cpp | fbzz::scene
// Selection outline render pass implementation
#include "SelectionPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/DrawCall.hpp>

namespace fbzz::scene {

void ExecuteSelectionOutlinePass(RenderPassContext& ctx)
{
    if (!ctx.selectionOutlineEnabled || !ctx.handles.outlineRT.IsValid()) return;

    auto& r = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h = ctx.handles;
    const auto& rs = ctx.settings;

    OutlineCB outlineData{};
    outlineData.color = {
        rs.outlineColor[0],
        rs.outlineColor[1],
        rs.outlineColor[2],
        rs.outlineColor[3]
    };
    outlineData.width = rs.outlineWidth;
    resources.Update(h.outlineCB, &outlineData, sizeof(OutlineCB));

    // SelectionOutline.hlsl はマスク探索の 1 タップ幅を b5 の texelSize で決める。
    // WHY: 直前に b5 を書いたパス頼みだと、ポストプロセスチェーンの構成次第で輪郭幅が変わる。
    const PostProcCB outlinePostData = MakeScreenPostProcCB(ctx.width, ctx.height);
    resources.Update(h.postprocCB, &outlinePostData, sizeof(PostProcCB));

    r.SetRenderTarget(rs.postProcess.fxaaEnabled ? h.outlineRT : ctx.outputRT, resources);

    renderer::DrawCall outlineDC;
    outlineDC.shader = h.selectionOutlineShader;
    outlineDC.pipelineState = h.postprocPSO;
    outlineDC.vertexCount = 3;
    outlineDC.constantBuffers[2] = h.outlineCB;
    outlineDC.constantBuffers[5] = h.postprocCB;
    outlineDC.textures[5] = h.postProcessInput.IsValid()
        ? h.postProcessInput
        : resources.GetColorTexture(h.ldrRT, 0);
    outlineDC.textures[6] = resources.GetColorTexture(h.selectionMaskRT, 0);
    outlineDC.textures[7] = resources.GetDepthTexture(h.hdrRT);
    outlineDC.textures[8] = resources.GetDepthTexture(h.selectionMaskRT);
    r.Submit(outlineDC, resources);

    h.fxaaInput = resources.GetColorTexture(h.outlineRT, 0);
}

} // namespace fbzz::scene
