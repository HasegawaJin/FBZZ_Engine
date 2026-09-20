/// @file    TAAPass.cpp
/// @brief   Temporal Anti-Aliasing (TAA) — 前フレームバッファと現フレームをブレンドして。
/// @author  Hasegawa Jin
/// @date    2026-06-23
///
/// @note TAA を選ぶ理由: MSAA はリアルタイムでは品質のトレードオフが大きく、FXAA は精細さを失う。
///       Halton ジッター + 前フレーム履歴でゼロコストに近い高品質 AA を実現する。
#include "PostProcessPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/DrawCall.hpp>

namespace fbzz::scene {

void ExecuteTAAPass(RenderPassContext& ctx)
{
    auto& r         = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;
    const auto& taa = ctx.settings.taa;

    if (!taa.enabled ||
        !h.taaShader.IsValid()    || !h.taaPSO.IsValid()    ||
        !h.taaHistoryA.IsValid()  || !h.taaHistoryB.IsValid())
        return;

    /// @note ping-pong: taaFlip=false → A が前フレーム履歴、B に書く。true ならその逆。
    ///       同一リソースを SRV/RTV に同時束縛できないため毎フレーム入れ替える。
    auto& historyRead  = h.taaFlip ? h.taaHistoryB : h.taaHistoryA;
    auto& historyWrite = h.taaFlip ? h.taaHistoryA : h.taaHistoryB;

    r.SetRenderTarget(historyWrite, resources);

    /// @note TAA.hlsl の近傍 3x3 variance clipping は b5 の texelSize を使う。未束縛だと DX12 は
    ///       null CBV で埋め texelSize=0 になり clip 範囲が潰れて TAA が無効化する
    ///       (DX11 は直前 Composite の束縛が残るため偶然動いていた)。
    const PostProcCB taaData = MakeScreenPostProcCB(ctx.width, ctx.height);
    resources.Update(h.postprocCB, &taaData, sizeof(PostProcCB));

    /// @note 入力: 現フレーム LDR (t5) + 前フレーム履歴 (t21=TEX_TAA_HISTORY)。variance clipping で
    ///       ゴーストを抑制し taaFeedback で重み付けブレンド。HDR を渡すと ACES トーンマップ前の
    ///       輝度で履歴ブレンドされ出力が暗くなるため、必ず ldrRT (Composite 出力) を使う。
    renderer::DrawCall taaDC;
    taaDC.shader          = h.taaShader;
    taaDC.pipelineState   = h.taaPSO;
    /// @note フルスクリーントライアングル
    taaDC.vertexCount     = 3;
    /// @note b0: CameraConstants (inv matrices)
    taaDC.constantBuffers[0] = h.frameCB;
    /// @note b5: texelSize (近傍サンプル幅)
    taaDC.constantBuffers[5] = h.postprocCB;
    /// @note b8: taaFeedback, prevViewProjection
    taaDC.constantBuffers[8] = h.advancedGraphicsCB;
    /// @note t5: 現フレーム LDR (Composite 出力)
    taaDC.textures[5]        = resources.GetColorTexture(ctx.Res().Target("LDR"), 0);
    /// @note t21: TEX_TAA_HISTORY (前フレーム)
    taaDC.textures[21]       = resources.GetColorTexture(historyRead, 0);
    /// @note t7: シーン深度。速度を書かない画素 (空・静止物) はこれでカメラの動きだけ再投影する。
    ///       束縛しないと無効な添字のディスクリプタを読む。
    taaDC.textures[7]        = resources.GetDepthTexture(ctx.Res().Target("HDR"));
    /// @note t26: モーションベクター。VelocityPass が動かなかったフレームは無効ハンドルのままで、
    ///       シェーダー側は B=0 を読んで従来の深度再投影へフォールバックする。
    if (ctx.Res().Target("Velocity").IsValid())
        taaDC.textures[26]   = resources.GetColorTexture(ctx.Res().Target("Velocity"), 0);
    /// @note t9: 粒子の反応マスク。粒子は速度を書かないので、覆われた画素は履歴を信じる割合を下げる。
    if (h.particleReactiveValid && h.particleReactiveRT != nullptr && h.particleReactiveRT->IsValid())
        taaDC.textures[9]    = resources.GetColorTexture(*h.particleReactiveRT, 0);
    r.Submit(taaDC, resources);

    /// @note フレーム終了後に ping-pong フラグを反転
    h.taaFlip = !h.taaFlip;
}

} // namespace fbzz::scene
