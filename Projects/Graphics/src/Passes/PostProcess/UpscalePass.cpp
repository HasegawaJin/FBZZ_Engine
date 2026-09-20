/// @file    UpscalePass.cpp
/// @brief   内部描画解像度の最終 LDR 画を出力先の実寸へ解像するパス。
/// @author  Hasegawa Jin
/// @date    2026-09-10
#include <Graphics/Passes/PostProcess/PostProcessPasses.hpp>
#include <Graphics/Pipeline/RenderPassContext.hpp>
#include <Graphics/Renderer/DrawCall.hpp>

namespace fbzz::renderer {

void ExecuteUpscalePass(RenderPassContext& ctx)
{
    auto& h = ctx.handles;

    if (!ctx.chainOutputRT.IsValid() || ctx.chainOutputRT == ctx.outputRT) return;

    const auto src = ctx.resources.GetColorTexture(ctx.chainOutputRT, 0);
    if (!src.IsValid()) return;

    /// @note 拡大は Catmull-Rom、縮小 (renderScale > 1 のスーパーサンプリング) は足跡の箱型平均。
    /// @note Catmull-Rom の負の重みは縮小側だとシャープ化になり、落としたエイリアスを縁に呼び戻すため。
    /// @note 双線形 1 タップの縮小は倍率が 2 を超えると入力を読み飛ばすので、平均する専用シェーダーを使う。
    const bool magnify = ctx.width < ctx.outputWidth || ctx.height < ctx.outputHeight;
    auto shader = h.copyColorShader;
    if (magnify && h.upscaleShader.IsValid())
        shader = h.upscaleShader;
    else if (!magnify && h.downscaleShader.IsValid())
        shader = h.downscaleShader;
    if (!shader.IsValid()) return;

    ctx.renderer.SetRenderTarget(ctx.outputRT, ctx.resources);

    /// @note b5 に入れるのは «入力» の寸法。Upscale.hlsl はこれでテクセル格子を組む。
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

} /// @note namespace fbzz::renderer
