/// @file    SSRPass.cpp
/// @brief   Screen Space Reflections パス — Compute Shader でレイマーチして映り込みを生成する。
/// @author  Hasegawa Jin
/// @date    2026-06-23
///
/// WHY: キューブマップ反射は静的シーンしか映せないが、SSR は動的オブジェクトも正確に映す。
/// GBuffer の法線・深度・金属度を活用するため、GBuffer 経路でのみ有効。
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

    // SSR はパイプライン名ではなく GBuffer の有無で判定する。
    // WHY: Forward 選択時も不透明物を GBuffer 経由にするため、反射の見た目を Deferred と揃えられる。
    if (!ssr.enabled ||
        !h.ssrShader.IsValid() || !h.ssrResult.IsValid() || !ctx.Res().Target("GBuffer").IsValid())
        return;

    // WHAT: GBuffer0(t0), HDR バッファ(t5), GBuffer1(t6), GBuffer 深度(t7),
    //       Forward 不透明物を含む HDR 深度(t25) を入力に、
    //       視点座標からの反射レイを View Space でトレースして映り込み色を UAV_SSR(u3) に書く。
    renderer::ComputeCall ssrDC;
    ssrDC.shader             = h.ssrShader;
    ssrDC.constantBuffers[0] = h.frameCB;             // b0: CameraConstants
    ssrDC.constantBuffers[8] = h.advancedGraphicsCB;  // b8: ssrMaxDistance, ssrThickness, ssrSteps, ssrIntensity
    ssrDC.srvInputs[0]       = resources.GetColorTexture(ctx.Res().Target("GBuffer"), 0); // t0: GBuffer0 (albedo + roughness)
    ssrDC.srvInputs[5]       = resources.GetColorTexture(ctx.Res().Target("HDR"), 0);     // t5: HDR color (TEX_GBUFFER0 スロット再利用)
    ssrDC.srvInputs[6]       = resources.GetColorTexture(ctx.Res().Target("GBuffer"), 1); // t6: GBuffer1 (normal + metallic)
    ssrDC.srvInputs[7]       = resources.GetDepthTexture(ctx.Res().Target("GBuffer"));    // t7: 反射面の GBuffer 深度
    ssrDC.srvInputs[25]      = resources.GetDepthTexture(ctx.Res().Target("HDR"));        // t25: Forward 不透明物を含むシーン深度
    ssrDC.uavOutputs[3]      = h.ssrResult;           // u3: UAV_SSR
    ssrDC.dispatchX          = (ctx.width  + 7) / 8;
    ssrDC.dispatchY          = (ctx.height + 7) / 8;
    ssrDC.dispatchZ          = 1;
    r.Dispatch(ssrDC, resources);
}

} // namespace fbzz::scene
