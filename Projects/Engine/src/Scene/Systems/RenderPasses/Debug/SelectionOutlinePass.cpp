/// @file    SelectionOutlinePass.cpp
/// @brief   Selection outline render pass implementation.
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include "SelectionPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/DrawCall.hpp>

namespace fbzz::scene {

void ExecuteSelectionOutlinePass(PassResources& res, RenderPassContext& ctx)
{
    if (!ctx.selectionOutlineEnabled) return;

    auto& r = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h = ctx.handles;
    const auto& rs = ctx.settings;
    /// @note Outline は FXAA へ中継するときだけ申告される。終端へ直書きするときは参照しない。
    const auto output = rs.postProcess.fxaaEnabled ? res.Target("Outline") : ctx.chainOutputRT;
    if (rs.postProcess.fxaaEnabled && !output.IsValid()) return;

    OutlineCB outlineData{};
    outlineData.color = {
        rs.outlineColor[0],
        rs.outlineColor[1],
        rs.outlineColor[2],
        rs.outlineColor[3]
    };
    /// @note シェーダーは内部解像度の画素で幅を数え、その後 UpscalePass が出力の実寸へ引き伸ばす。
    /// @note 描画スケールで太さが変わらないよう、出力の画素で見た幅になるよう換算する。
    const float renderToOutput = ctx.outputWidth > 0u
        ? static_cast<float>(ctx.width) / static_cast<float>(ctx.outputWidth) : 1.0f;
    outlineData.width = rs.outlineWidth * renderToOutput;
    resources.Update(h.outlineCB, &outlineData, sizeof(OutlineCB));

    /// @note `SelectionOutline.hlsl` はマスク探索の 1 タップ幅を b5 の texelSize で決める。直前に b5 を書いたパス頼みだと、ポストプロセスチェーンの構成次第で輪郭幅が変わる。
    const PostProcCB outlinePostData = MakeScreenPostProcCB(ctx.width, ctx.height);
    resources.Update(h.postprocCB, &outlinePostData, sizeof(PostProcCB));

    r.SetRenderTarget(output, resources);

    renderer::DrawCall outlineDC;
    outlineDC.shader = h.selectionOutlineShader;
    outlineDC.pipelineState = h.postprocPSO;
    outlineDC.vertexCount = 3;
    outlineDC.constantBuffers[2] = h.outlineCB;
    outlineDC.constantBuffers[5] = h.postprocCB;
    outlineDC.textures[5] = h.postProcessInput.IsValid()
        ? h.postProcessInput
        : resources.GetColorTexture(ctx.Res().Target("LDR"), 0);
    outlineDC.textures[6] = resources.GetColorTexture(res.Target("SelectionMask"), 0);
    outlineDC.textures[7] = resources.GetDepthTexture(ctx.Res().Target("HDR"));
    outlineDC.textures[8] = resources.GetDepthTexture(res.Target("SelectionMask"));
    r.Submit(outlineDC, resources);

    h.fxaaInput = resources.GetColorTexture(output, 0);
}

} /// @note namespace fbzz::scene
