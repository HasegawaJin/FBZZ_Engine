/// @file    SelectionOutlinePass.cpp
/// @brief   選択マスクから輪郭を描く。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include <Graphics/Renderer/Commands/ShaderBindingSlots.hpp>
#include "SelectionPasses.hpp"
#include <Engine/Rendering/Passes/RenderPassContext.hpp>
#include <Graphics/Renderer/Commands/DrawCall.hpp>

namespace fbzz::scene {

void ExecuteSelectionOutlinePass(renderer::PassResources& res, RenderPassContext& ctx)
{
    if (!ctx.selectionOutlineEnabled) {
        return;
    }

    auto& r = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h = ctx.handles;
    const auto& rs = ctx.settings;
    // Outline は FXAA へ中継するときだけ申告される。終端へ直書きするときは参照しない。
    const auto output = rs.postProcess.fxaaEnabled ? res.Target("Outline") : ctx.chainOutputRT;
    if (rs.postProcess.fxaaEnabled && !output.IsValid()) {
        return;
    }

    OutlineCB outlineData{};
    outlineData.color = {
        rs.outlineColor[0],
        rs.outlineColor[1],
        rs.outlineColor[2],
        rs.outlineColor[3]
    };
    // シェーダーは内部解像度の画素で幅を数え、その後 UpscalePass が出力の実寸へ引き伸ばす。
    // 描画スケールで太さが変わらないよう、出力の画素で見た幅になるよう換算する。
    const float renderToOutput = ctx.outputWidth > 0u
        ? static_cast<float>(ctx.width) / static_cast<float>(ctx.outputWidth) : 1.0f;
    outlineData.width = rs.outlineWidth * renderToOutput;
    resources.Update(h.outlineCB, &outlineData, sizeof(OutlineCB));

    // `SelectionOutline.hlsl` はマスク探索の 1 タップ幅を b5 の texelSize で決める。直前に b5 を書いたパス頼みだと、ポストプロセスチェーンの構成次第で輪郭幅が変わる。
    const PostProcCB outlinePostData = MakeScreenPostProcCB(ctx.width, ctx.height);
    resources.Update(h.postprocCB, &outlinePostData, sizeof(PostProcCB));

    r.SetRenderTarget(output, resources);

    renderer::DrawCall outlineDC;
    outlineDC.shader = h.selectionOutlineShader;
    outlineDC.pipelineState = h.postprocPSO;
    outlineDC.vertexCount = 3;
    outlineDC.constantBuffers[renderer::cb::MATERIAL] = h.outlineCB;
    outlineDC.constantBuffers[renderer::cb::POSTPROC] = h.postprocCB;
    outlineDC.textures[renderer::srv::POST_COLOR] = h.postProcessInput.IsValid()
        ? h.postProcessInput
        : resources.GetColorTexture(ctx.Res().Target("LDR"), 0);
    outlineDC.textures[renderer::srv::MASK_COLOR] = resources.GetColorTexture(res.Target("SelectionMask"), 0);
    outlineDC.textures[renderer::srv::DEPTH] = resources.GetDepthTexture(ctx.Res().Target("HDR"));
    outlineDC.textures[renderer::srv::MASK_DEPTH] = resources.GetDepthTexture(res.Target("SelectionMask"));
    r.Submit(outlineDC, resources);

    h.fxaaInput = resources.GetColorTexture(output, 0);
}

} // namespace fbzz::scene
