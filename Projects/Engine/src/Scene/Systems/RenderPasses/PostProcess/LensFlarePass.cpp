/// @file LensFlarePass.cpp
/// @brief スクリーンスペースレンズフレアを HDR バッファへ加算合成するパス
/// @author Hasegawa Jin
/// @date 2026/06/23

#include "PostProcessPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/ComputeCall.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Renderer/SamplerMode.hpp>
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

    PostProcCB flareData = MakeScreenPostProcCB(ctx.width, ctx.height);
    // 光源抽出は Bloom と同じ CS を使い回すので、しきい値も Bloom の設定に合わせる。
    flareData.bloomThreshold = rs.postProcess.bloom.threshold;
    flareData.bloomSoftKnee  = rs.postProcess.bloom.softKnee;
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
    // フレアは輝点を画面反対側へ写して拾うため、端の引き伸ばしが混ざらない clamp が要る。
    r.SetSampler(0, renderer::SamplerMode::CLAMP_LINEAR);

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
