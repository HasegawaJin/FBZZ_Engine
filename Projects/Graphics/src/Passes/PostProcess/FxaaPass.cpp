/// @file    FxaaPass.cpp
/// @brief   ポストプロセスチェーンの最新画像を FXAA で最終出力へ描く。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include <Graphics/Renderer/Commands/ShaderBindingSlots.hpp>
#include <Graphics/Passes/PostProcess/PostProcessPasses.hpp>
#include <Graphics/Pipeline/Passes/RenderPassContext.hpp>
#include <Graphics/Renderer/Commands/DrawCall.hpp>

namespace fbzz::renderer {

void ExecuteFxaaPass(RenderPassContext& ctx)
{
    auto& h = ctx.handles;
    const auto& rs = ctx.settings;

    // CustomPP / Outline 後は LDR ではなく、グラフが申告した直前の出力を読む。
    if (rs.postProcess.fxaaEnabled && h.fxaaShader.IsValid() && h.fxaaInput.IsValid())
    {
        ctx.renderer.SetRenderTarget(ctx.chainOutputRT, ctx.resources);

        // FXAA.hlsl はサンプル間隔を b5 の texelSize だけで決めるため、ここで自前で
        // 入れる。直前に b5 を書いたのが Composite か CustomPostProcess かでたまたま
        // 正しい値が残っていただけの依存だと、チェーンの構成が変われば静かに壊れる。
        const PostProcCB fxaaData = MakeScreenPostProcCB(ctx.width, ctx.height);
        ctx.resources.Update(h.postprocCB, &fxaaData, sizeof(PostProcCB));

        renderer::DrawCall fxaaDC;
        fxaaDC.shader = h.fxaaShader;
        fxaaDC.pipelineState = h.postprocPSO;
        fxaaDC.vertexCount = 3;
        fxaaDC.constantBuffers[cb::POSTPROC] = h.postprocCB;
        fxaaDC.textures[srv::POST_COLOR] = h.fxaaInput;
        ctx.renderer.Submit(fxaaDC, ctx.resources);
    }
}

void ExecuteTAABlitPass(RenderPassContext& ctx)
{
    auto& h = ctx.handles;

    // TAA 出力 (fxaaInput = taaHistoryA or B) を OutputRT に blit する。TAA は
    // ping-pong 履歴バッファにのみ書き OutputRT には書かないため、後続に FXAA が
    // ない場合 OutputRT が更新されず viewport が黒になる。
    // 素通しのコピーにする。FXAA を流用すると、FXAA を切っていても TAA の絵がもう一段ぼける。
    if (!h.copyColorShader.IsValid() || !h.fxaaInput.IsValid()) {
        return;
    }

    ctx.renderer.SetRenderTarget(ctx.chainOutputRT, ctx.resources);

    const PostProcCB blitData = MakeScreenPostProcCB(ctx.width, ctx.height);
    ctx.resources.Update(h.postprocCB, &blitData, sizeof(PostProcCB));

    renderer::DrawCall blitDC;
    blitDC.shader             = h.copyColorShader;
    blitDC.pipelineState      = h.postprocPSO;
    blitDC.vertexCount        = 3;
    blitDC.constantBuffers[cb::POSTPROC] = h.postprocCB;
    // TAA 出力 (taaHistoryA or B)
    blitDC.textures[srv::POST_COLOR]        = h.fxaaInput;
    ctx.renderer.Submit(blitDC, ctx.resources);
}

} // namespace fbzz::renderer
