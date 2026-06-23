// FBZZ Engine
// MotionBlurPass.cpp | fbzz::scene
// カメラモーションブラー — 深度再投影で各ピクセルのモーションベクトルを求め、
// そのベクトル方向にサンプルを積算してブレを表現する Compute パス。
// WHY: オブジェクトモーションブラーは MRT のベロシティバッファが必要だが、
//      カメラブラーは深度と前フレームの ViewProjection 行列だけで実装できる。
//      高速移動・旋回時のシネマティックな残像感を付加するため導入する。
#include "PostProcessPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/ComputeCall.hpp>

namespace fbzz::scene {

void ExecuteMotionBlurPass(RenderPassContext& ctx)
{
    auto& r       = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h       = ctx.handles;
    const auto& mb = ctx.settings.motionBlur;

    if (!mb.enabled ||
        !h.motionBlurShader.IsValid() || !h.motionBlurResult.IsValid())
        return;

    // WHAT: 深度バッファ(t7)をサンプルしてピクセルのワールド位置を復元し、
    //       前フレームの ViewProjection 行列で再投影してモーションベクトルを計算する。
    //       ベクトル方向に motionBlurSamples 点を中心対称サンプルして UAV_MOTION_BLUR(u5) に書く。
    renderer::ComputeCall mbDC;
    mbDC.shader             = h.motionBlurShader;
    mbDC.constantBuffers[0] = h.frameCB;            // b0: CameraConstants (invViewProjection)
    mbDC.constantBuffers[8] = h.advancedGraphicsCB; // b8: motionBlurStrength, motionBlurSamples, prevViewProjection
    mbDC.srvInputs[5]       = resources.GetColorTexture(h.hdrRT, 0);  // t5: 現フレーム HDR カラー
    mbDC.srvInputs[7]       = resources.GetDepthTexture(
        ctx.isDeferred ? h.gbufferRT : h.hdrRT);                       // t7: Depth
    mbDC.uavOutputs[5]      = h.motionBlurResult;  // u5: UAV_MOTION_BLUR
    mbDC.dispatchX          = (ctx.width  + 7) / 8;
    mbDC.dispatchY          = (ctx.height + 7) / 8;
    mbDC.dispatchZ          = 1;
    r.Dispatch(mbDC, resources);
}

} // namespace fbzz::scene
