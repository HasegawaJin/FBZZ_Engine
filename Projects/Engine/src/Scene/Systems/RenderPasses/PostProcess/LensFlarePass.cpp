/// @file LensFlarePass.cpp
/// @brief スクリーンスペースレンズフレアを HDR バッファへ加算合成するパス
/// @author Hasegawa Jin
/// @date 2026/06/23

#include "PostProcessPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/ComputeCall.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <algorithm>

namespace fbzz::scene {

void ExecuteLensFlarePass(RenderPassContext& ctx)
{
    auto& r         = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;
    const auto& rs  = ctx.settings;
    const auto& lf  = rs.lensFlare;

    if (!lf.enabled || lf.intensity <= 0.0f ||
        !h.lensFlareShader.IsValid() || !h.lensFlarePSO.IsValid() ||
        !h.bloomDownShader.IsValid() ||
        !h.bloomHalf.IsValid()       || !h.hdrRT.IsValid())
        return;

    // 光源抽出は Bloom と同じ CS を使い回すので、CB も BloomDownsample の規約で埋める。
    //   texelSize     = 書き込み先 (bloomHalf = 半解像度)
    //   bloomSrcTexel = 読み込み元 (HDR = 全解像度)
    // WHY 全解像度で埋めてはいけないか: 新しい BloomDownsample は texelSize から
    //     直接 UV を作る。全解像度の値を渡すと UV が半分になり、画面左上 1/4 を
    //     引き伸ばしたものが抽出結果になる。
    const uint32_t halfW = std::max(1u, ctx.width  / 2);
    const uint32_t halfH = std::max(1u, ctx.height / 2);
    PostProcCB flareData = MakeScreenPostProcCB(halfW, halfH);
    // screenSize だけは LensFlare の PS が縦横比に使うので全解像度のまま渡す。
    flareData.screenSize[0] = static_cast<float>(ctx.width);
    flareData.screenSize[1] = static_cast<float>(ctx.height);
    flareData.bloomSrcTexel[0] = 1.0f / static_cast<float>(std::max(1u, ctx.width));
    flareData.bloomSrcTexel[1] = 1.0f / static_cast<float>(std::max(1u, ctx.height));
    flareData.bloomThreshold = rs.postProcess.bloom.threshold;
    flareData.bloomSoftKnee  = rs.postProcess.bloom.softKnee;
    // 輝点の選別が本パスの目的なので、閾値は必ず掛ける。
    flareData.bloomApplyThreshold = 1.0f;
    flareData.bloomAdditive       = 0.0f;
    // BloomDownsample は bloomIntensity <= 0 を「Bloom 無効」と見なして出力をゼロ埋めする。
    flareData.bloomIntensity = 1.0f;
    resources.Update(h.postprocCB, &flareData, sizeof(PostProcCB));

    // WHY: 本パスは Bloom より前に走る (フレアを Bloom に乗せるため)。つまり bloomHalf には
    //      今フレームの輝点がまだ無く、Bloom 自体が無効なら一度も書かれない。他パスと共有の
    //      バッファに自前で輝度抽出を焼き、後段の Bloom が上書きする前に読み切る。
    renderer::ComputeCall brightDC;
    brightDC.shader             = h.bloomDownShader;
    brightDC.constantBuffers[5] = h.postprocCB;
    brightDC.srvInputs[10]      = resources.GetColorTexture(h.hdrRT, 0);
    brightDC.uavOutputs[0]      = h.bloomHalf;
    brightDC.dispatchX = (std::max(1u, ctx.width  / 2) + 7) / 8;
    brightDC.dispatchY = (std::max(1u, ctx.height / 2) + 7) / 8;
    brightDC.dispatchZ = 1;
    r.Dispatch(brightDC, resources);

    r.SetRenderTarget(h.hdrRT, resources);

    renderer::DrawCall flareDC;
    flareDC.shader             = h.lensFlareShader;
    flareDC.pipelineState      = h.lensFlarePSO;
    flareDC.vertexCount        = 3;
    flareDC.constantBuffers[5] = h.postprocCB;
    flareDC.constantBuffers[8] = h.advancedGraphicsCB;
    flareDC.textures[5]        = h.bloomHalf;
    r.Submit(flareDC, resources);
}

} // namespace fbzz::scene
