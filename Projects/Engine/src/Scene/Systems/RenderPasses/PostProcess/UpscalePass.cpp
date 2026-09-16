/// @file    UpscalePass.cpp
/// @brief   内部描画解像度の最終 LDR 画を出力先の実寸へ解像するパス。
/// @author  Hasegawa Jin
/// @date    2026-09-10
#include "PostProcessPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/DrawCall.hpp>

namespace fbzz::scene {

void ExecuteUpscalePass(RenderPassContext& ctx)
{
    auto& h = ctx.handles;

    if (!ctx.chainOutputRT.IsValid() || ctx.chainOutputRT == ctx.outputRT) return;

    const auto src = ctx.resources.GetColorTexture(ctx.chainOutputRT, 0);
    if (!src.IsValid()) return;

    // 拡大は Catmull-Rom、縮小 (renderScale > 1 のスーパーサンプリング) は素の linear。
    // WHY: Catmull-Rom の負の重みは «入力の方が細かい» 側では単なるシャープ化になり、
    //      せっかく落としたエイリアスを縁に呼び戻す。
    const bool magnify = ctx.width < ctx.outputWidth || ctx.height < ctx.outputHeight;
    const auto shader  = (magnify && h.upscaleShader.IsValid()) ? h.upscaleShader : h.copyColorShader;
    if (!shader.IsValid()) return;

    ctx.renderer.SetRenderTarget(ctx.outputRT, ctx.resources);

    // b5 に入れるのは «入力» の寸法。Upscale.hlsl はこれでテクセル格子を組む。
    const PostProcCB upscaleData = MakeScreenPostProcCB(ctx.width, ctx.height);
    ctx.resources.Update(h.postprocCB, &upscaleData, sizeof(PostProcCB));

    renderer::DrawCall dc;
    dc.shader             = shader;
    dc.pipelineState      = h.postprocPSO;
    dc.vertexCount        = 3;
    dc.constantBuffers[5] = h.postprocCB;
    dc.textures[5]        = src;
    ctx.renderer.Submit(dc, ctx.resources);
}

} // namespace fbzz::scene
