// FBZZ Engine
// LensFlarePass.cpp | fbzz::scene
// スクリーンスペースレンズフレア — フルスクリーン PS で光源周辺のゴーストとハローを
// HDR バッファに加算合成する。
// WHY: 物理ベースのレンズシミュレーションはコストが高い。
//      スクリーンスペースでの加算合成は低コストで映画的なカメラ効果を表現できる。
//      入力を bloom の明るい領域バッファ (bloomHalf) にすることで輝度閾値処理を省略し、
//      SRV/RTV 競合も回避できる（bloomHalf ≠ hdrRT）。
#include "PostProcessPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/DrawCall.hpp>

namespace fbzz::scene {

void ExecuteLensFlarePass(RenderPassContext& ctx)
{
    auto& r         = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;
    const auto& lf  = ctx.settings.lensFlare;

    if (!lf.enabled ||
        !h.lensFlareShader.IsValid() || !h.lensFlarePSO.IsValid() ||
        !h.bloomHalf.IsValid()       || !h.hdrRT.IsValid())
        return;

    // WHAT: hdrRT を RTV にセットし、ADDITIVE ブレンドの PSO でレンズフレアを加算合成する。
    //       入力は bloomHalf (t5) — ダウンサンプル済みの HDR 明るい領域テクスチャ。
    //       bloomHalf は hdrRT とは別リソースのため SRV/RTV 競合が起きない。
    r.SetRenderTarget(h.hdrRT, resources);

    // WHAT: フルスクリーントライアングル 1 枚を ADDITIVE PSO で描画する。
    //       PS 内でゴースト(lensFlareGhostCount 個)とハローを生成し、
    //       輝度 > 1.0 のピクセルのみ寄与させることで不自然な広がりを防ぐ。
    renderer::DrawCall flareDC;
    flareDC.shader          = h.lensFlareShader;
    flareDC.pipelineState   = h.lensFlarePSO;
    flareDC.vertexCount     = 3;    // フルスクリーントライアングル
    flareDC.constantBuffers[5] = h.postprocCB;        // b5: texelSize, screenSize
    flareDC.constantBuffers[8] = h.advancedGraphicsCB; // b8: lensFlareIntensity/ghostCount/haloWidth/distort
    flareDC.textures[5]        = h.bloomHalf;         // t5: 明るい領域ソース
    r.Submit(flareDC, resources);
}

} // namespace fbzz::scene
