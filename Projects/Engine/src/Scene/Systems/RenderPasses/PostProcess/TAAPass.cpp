// FBZZ Engine
// TAAPass.cpp | fbzz::scene
// Temporal Anti-Aliasing (TAA) — 前フレームバッファと現フレームをブレンドして
// サブピクセルエイリアスを除去する。Halton ジッターとの組み合わせで収束を高速化する。
// WHY: MSAA はリアルタイム品質のトレードオフが大きく、FXAA は精細さを失う。
//      TAA は前フレームの情報を活用してゼロコストに近い高品質 AA を実現する。
//      履歴バッファを ping-pong する理由: 同一 RT を SRV と RTV に同時束縛できないため。
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

    // WHAT: 現フレーム LDR (t5) + 前フレーム履歴 (t21=TEX_TAA_HISTORY) を入力に
    //       variance clipping でゴーストを抑制しながら taaFeedback で重み付けブレンド。
    // WHY: TAA は Composite (トーンマップ後) の LDR 出力に対して適用する。
    //      HDR バッファを渡すと ACES トーンマップを経由しない輝度で履歴ブレンドが行われ、
    //      出力が若干暗く見える。ldrRT (Composite 出力) を使うことで正しい輝度になる。
    renderer::DrawCall taaDC;
    taaDC.shader          = h.taaShader;
    taaDC.pipelineState   = h.taaPSO;
    taaDC.vertexCount     = 3;    // フルスクリーントライアングル
    taaDC.constantBuffers[0] = h.frameCB;            // b0: CameraConstants (jitter, inv matrices)
    taaDC.constantBuffers[8] = h.advancedGraphicsCB; // b8: taaFeedback, taaJitterX/Y, prevViewProjection
    taaDC.textures[5]        = resources.GetColorTexture(h.ldrRT, 0);   // t5: 現フレーム LDR (Composite 出力)
    taaDC.textures[21]       = resources.GetColorTexture(historyRead, 0); // t21: TEX_TAA_HISTORY (前フレーム)
    r.Submit(taaDC, resources);

    // フレーム終了後に ping-pong フラグを反転
    h.taaFlip = !h.taaFlip;
}

} // namespace fbzz::scene
