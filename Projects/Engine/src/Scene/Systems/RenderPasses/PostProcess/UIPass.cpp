/// @file    UIPass.cpp
/// @brief   UI を最終出力 RT へ合成するパス。
/// @author  Hasegawa Jin
/// @date    2026-09-10
/// @note /// UI は «最終フレームへの合成» なので、どの分岐が最後に Output を書いたかに
/// @note 依存してはいけない。Output を ReadWrite するパスとして登録し、ここで明示的に
/// @note outputRT を束縛する。
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
    /// @note ポストプロセスの最終 RT はシーン深度を持たない。実寸へ転写してから UI をテストする。
    /// @note Output と HDR の解像度は renderScale によって異なるため、画素番号ではなく UV で写す。
    auto* output = ctx.resources.Get(ctx.outputRT);
    ui->context->reversedDepth = output && output->IsReversedZ();
    const bool outputHasDepth = !ctx.outputRT.IsValid() || ctx.resources.GetDepthTexture(ctx.outputRT).IsValid();
    ui->context->sceneDepthAvailable = outputHasDepth && UICopySceneDepth(ctx.renderer, ctx.resources, *ui->context,
        ctx.resources.GetDepthTexture(ctx.Res().Target("HDR")), ui->context->reversedDepth);

    /// @note UI はポストプロセス後に outputRT へ直接描くので描画スケールの影響を受けない。
    /// @note 内部解像度を使うと renderScale < 1 でレイアウトだけ縮み、UI が左上に寄る。
    const float uiWidth = ui->viewportWidth > 0.0f
        ? ui->viewportWidth
        : static_cast<float>(ctx.outputWidth);
    const float uiHeight = ui->viewportHeight > 0.0f
        ? ui->viewportHeight
        : static_cast<float>(ctx.outputHeight);

    /// @note デバッグ表示の可否は RenderSettings が持つ。UISystem は設定の
    /// @note 所有者を知らない自由関数なので、知っている側が毎フレーム入れる。
    ui->context->showRects = ctx.settings.showUIRects;
    ui->context->gameplayInput = ui->gameplayInput;

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

} /// @note namespace fbzz::scene
