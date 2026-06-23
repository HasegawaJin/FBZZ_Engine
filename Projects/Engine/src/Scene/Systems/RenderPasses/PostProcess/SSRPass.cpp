// FBZZ Engine
// SSRPass.cpp | fbzz::scene
// Screen Space Reflections パス — Compute Shader でレイマーチして映り込みを生成する。
// WHY: キューブマップ反射は静的シーンしか映せないが、SSR は動的オブジェクトも正確に映す。
//      Deferred GBuffer の法線・深度・金属度を活用するため、Deferred パイプラインでのみ有効。
#include "PostProcessPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/ComputeCall.hpp>

namespace fbzz::scene {

void ExecuteSSRPass(RenderPassContext& ctx)
{
    auto& r         = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;
    const auto& ssr = ctx.settings.ssr;

    // SSR は Deferred パイプライン専用（GBuffer が必要）
    if (!ssr.enabled || !ctx.isDeferred ||
        !h.ssrShader.IsValid() || !h.ssrResult.IsValid() || !h.gbufferRT.IsValid())
        return;

    // WHAT: GBuffer0(t0), HDR バッファ(t5), GBuffer1(t6), GBuffer 深度(t7),
    //       Forward 不透明物を含む HDR 深度(t25) を入力に、
    //       視点座標からの反射レイを View Space でトレースして映り込み色を UAV_SSR(u3) に書く。
    renderer::ComputeCall ssrDC;
    ssrDC.shader             = h.ssrShader;
    ssrDC.constantBuffers[0] = h.frameCB;             // b0: CameraConstants
    ssrDC.constantBuffers[8] = h.advancedGraphicsCB;  // b8: ssrMaxDistance, ssrThickness, ssrSteps, ssrIntensity
    ssrDC.srvInputs[0]       = resources.GetColorTexture(h.gbufferRT, 0); // t0: GBuffer0 (albedo + roughness)
    ssrDC.srvInputs[5]       = resources.GetColorTexture(h.hdrRT, 0);     // t5: HDR color (TEX_GBUFFER0 スロット再利用)
    ssrDC.srvInputs[6]       = resources.GetColorTexture(h.gbufferRT, 1); // t6: GBuffer1 (normal + metallic)
    ssrDC.srvInputs[7]       = resources.GetDepthTexture(h.gbufferRT);    // t7: 反射面の GBuffer 深度
    ssrDC.srvInputs[25]      = resources.GetDepthTexture(h.hdrRT);        // t25: Forward 不透明物を含むシーン深度
    ssrDC.uavOutputs[3]      = h.ssrResult;           // u3: UAV_SSR
    ssrDC.dispatchX          = (ctx.width  + 7) / 8;
    ssrDC.dispatchY          = (ctx.height + 7) / 8;
    ssrDC.dispatchZ          = 1;
    r.Dispatch(ssrDC, resources);
}

} // namespace fbzz::scene
