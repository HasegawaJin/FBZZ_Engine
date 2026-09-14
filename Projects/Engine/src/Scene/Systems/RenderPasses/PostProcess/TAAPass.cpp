/// @file    TAAPass.cpp
/// @brief   Temporal Anti-Aliasing (TAA) — 前フレームバッファと現フレームをブレンドして。
/// @author  Hasegawa Jin
/// @date    2026-06-23
///
/// サブピクセルエイリアスを除去する。Halton ジッターとの組み合わせで収束を高速化する。
/// WHY: MSAA はリアルタイム品質のトレードオフが大きく、FXAA は精細さを失う。
/// TAA は前フレームの情報を活用してゼロコストに近い高品質 AA を実現する。
/// 履歴バッファを ping-pong する理由: 同一 RT を SRV と RTV に同時束縛できないため。
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

    // ping-pong: taaFlip=false → A が前フレーム履歴, B に書く
    //            taaFlip=true  → B が前フレーム履歴, A に書く
    // WHY: DX11 は同一リソースを SRV/RTV に同時にバインドできないため、
    //      書き込み先と読み取り元を毎フレーム入れ替える。
    auto& historyRead  = h.taaFlip ? h.taaHistoryB : h.taaHistoryA;
    auto& historyWrite = h.taaFlip ? h.taaHistoryA : h.taaHistoryB;

    // WHAT: 書き込み先の history バッファを RT としてセット
    r.SetRenderTarget(historyWrite, resources);

    // TAA.hlsl は近傍 3x3 の variance clipping に b5 の texelSize を使う。
    // WHY: 束縛しないと DX12 は未指定スロットを null CBV で埋めるため texelSize が 0 になり、
    //      近傍 8 点が全て自分自身になって clip 範囲が潰れ、履歴が毎フレーム現フレーム色へ
    //      丸められる = TAA が何もしなくなる。DX11 は直前の Composite の束縛が残るため
    //      たまたま動いていた。
    const PostProcCB taaData = MakeScreenPostProcCB(ctx.width, ctx.height);
    resources.Update(h.postprocCB, &taaData, sizeof(PostProcCB));

    // WHAT: 現フレーム LDR (t5) + 前フレーム履歴 (t21=TEX_TAA_HISTORY) を入力に
    //       variance clipping でゴーストを抑制しながら taaFeedback で重み付けブレンド。
    // WHY: TAA は Composite (トーンマップ後) の LDR 出力に対して適用する。
    //      HDR バッファを渡すと ACES トーンマップを経由しない輝度で履歴ブレンドが行われ、
    //      出力が若干暗く見える。ldrRT (Composite 出力) を使うことで正しい輝度になる。
    renderer::DrawCall taaDC;
    taaDC.shader          = h.taaShader;
    taaDC.pipelineState   = h.taaPSO;
    taaDC.vertexCount     = 3;    // フルスクリーントライアングル
    taaDC.constantBuffers[0] = h.frameCB;            // b0: CameraConstants (inv matrices)
    taaDC.constantBuffers[5] = h.postprocCB;         // b5: texelSize (近傍サンプル幅)
    taaDC.constantBuffers[8] = h.advancedGraphicsCB; // b8: taaFeedback, prevViewProjection
    taaDC.textures[5]        = resources.GetColorTexture(ctx.Res().Target("LDR"), 0);   // t5: 現フレーム LDR (Composite 出力)
    taaDC.textures[21]       = resources.GetColorTexture(historyRead, 0); // t21: TEX_TAA_HISTORY (前フレーム)
    // t26: モーションベクター。VelocityPass が動かなかったフレームは無効ハンドルのままで、
    //      シェーダー側は B=0 を読んで従来の深度再投影へフォールバックする。
    if (ctx.Res().Target("Velocity").IsValid())
        taaDC.textures[26]   = resources.GetColorTexture(ctx.Res().Target("Velocity"), 0);
    // t9: 粒子の反応マスク。粒子は速度を書かないので、覆われた画素は履歴を信じる割合を下げる。
    if (h.particleReactiveValid && h.particleReactiveRT != nullptr && h.particleReactiveRT->IsValid())
        taaDC.textures[9]    = resources.GetColorTexture(*h.particleReactiveRT, 0);
    r.Submit(taaDC, resources);

    // フレーム終了後に ping-pong フラグを反転
    h.taaFlip = !h.taaFlip;
}

} // namespace fbzz::scene
