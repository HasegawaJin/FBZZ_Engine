/// @file    UIPass.cpp
/// @brief   UI を最終出力 RT へ合成するパス。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// UI は «最終フレームへの合成» なので、どの分岐が最後に Output を書いたかに
/// 依存してはいけない。Output を ReadWrite するパスとして登録し、ここで明示的に
/// outputRT を束縛する。
#include "PostProcessPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Scene/Systems/RenderSystem.hpp>
#include <Engine/Scene/Systems/UISystem.hpp>

namespace fbzz::scene {

void ExecuteUIPass(RenderPassContext& ctx)
{
    const RenderSystemUIOptions* ui = ctx.uiOptions;
    if (!ui || !ui->enabled || !ui->context) return;

    ctx.renderer.SetRenderTarget(ctx.outputRT, ctx.resources);

    /// @note UI はポストプロセス後に outputRT へ直接描くので描画スケールの影響を受けない。
    ///       内部解像度を使うと renderScale < 1 でレイアウトだけ縮み、UI が左上に寄る。
    const float uiWidth = ui->viewportWidth > 0.0f
        ? ui->viewportWidth
        : static_cast<float>(ctx.outputWidth);
    const float uiHeight = ui->viewportHeight > 0.0f
        ? ui->viewportHeight
        : static_cast<float>(ctx.outputHeight);

    /// @note デバッグ表示の可否は RenderSettings が持つ。UISystem は設定の
    ///       所有者を知らない自由関数なので、知っている側が毎フレーム入れる。
    ui->context->showRects = ctx.settings.showUIRects;

    UISystem(ctx.scene,
             ctx.renderer,
             ctx.resources,
             *ui->context,
             uiWidth,
             uiHeight,
             ui->mouseInCanvasSpace,
             ui->mousePressed,
             ctx.camera.m_position,
             ctx.camera.m_rotation,
             ctx.camera.GetViewProjection(),
             ui->targetView);
}

} // namespace fbzz::scene
