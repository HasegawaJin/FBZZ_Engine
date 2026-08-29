/// @file    FxaaPass.cpp
/// @brief   FXAA render pass implementation.
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include "PostProcessPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/DrawCall.hpp>

namespace fbzz::scene {

void ExecuteFxaaPass(RenderPassContext& ctx)
{
    auto& h = ctx.handles;
    const auto& rs = ctx.settings;

    if (rs.postProcess.fxaaEnabled && h.fxaaShader.IsValid() && h.ldrRT.IsValid())
    {
        ctx.renderer.SetRenderTarget(ctx.outputRT, ctx.resources);

        // FXAA.hlsl はサンプル間隔を b5 の texelSize だけで決める。
        // WHY: 直前に b5 を書いたのが Composite か CustomPostProcess かでたまたま正しい値が
        //      残っていただけで、チェーンの構成が変われば静かに壊れる。自前で入れる。
        const PostProcCB fxaaData = MakeScreenPostProcCB(ctx.width, ctx.height);
        ctx.resources.Update(h.postprocCB, &fxaaData, sizeof(PostProcCB));

        renderer::DrawCall fxaaDC;
        fxaaDC.shader = h.fxaaShader;
        fxaaDC.pipelineState = h.postprocPSO;
        fxaaDC.vertexCount = 3;
        fxaaDC.constantBuffers[5] = h.postprocCB;
        fxaaDC.textures[5] = h.fxaaInput;
        ctx.renderer.Submit(fxaaDC, ctx.resources);
    }
}

void ExecuteTAABlitPass(RenderPassContext& ctx)
{
    auto& h = ctx.handles;

    // TAA 出力 (fxaaInput = taaHistoryA or B) を OutputRT に blit する。
    // WHY: TAA は ping-pong 履歴バッファにのみ書き OutputRT には書かない。
    //      後続に FXAA がない場合 OutputRT が更新されず viewport が黒になるため、
    //      FXAA シェーダーを blit として流用して最終出力に届ける。
    //      TAA 後の出力はエッジがほぼ平滑化済みなので FXAA の追加処理量は極小。
    if (!h.fxaaShader.IsValid() || !h.fxaaInput.IsValid()) return;

    ctx.renderer.SetRenderTarget(ctx.outputRT, ctx.resources);

    const PostProcCB blitData = MakeScreenPostProcCB(ctx.width, ctx.height);
    ctx.resources.Update(h.postprocCB, &blitData, sizeof(PostProcCB));

    renderer::DrawCall blitDC;
    blitDC.shader             = h.fxaaShader;
    blitDC.pipelineState      = h.postprocPSO;
    blitDC.vertexCount        = 3;
    blitDC.constantBuffers[5] = h.postprocCB;
    blitDC.textures[5]        = h.fxaaInput;   // TAA 出力 (taaHistoryA or B)
    ctx.renderer.Submit(blitDC, ctx.resources);
}

} // namespace fbzz::scene
