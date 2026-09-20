/// @file    MotionBlurPass.cpp
/// @brief   モーションブラー — 各ピクセルの移動量ぶんサンプルを積算してブレを表現する Compute パス。
/// @author  Hasegawa Jin
/// @date    2026-06-23

/// @note 移動量の出どころは 2 つある:
/// @note 1. VelocityPass が描いたモーションベクター (t26) — カメラとオブジェクト両方の動き
/// @note 2. 深度 + prevViewProjection の再投影 — カメラの動きだけ。1 が無い画素の穴埋め
/// @note 判定はシェーダー側で行う (velocity.b が書き込み済みフラグ)。
#include <Graphics/Passes/PostProcess/PostProcessPasses.hpp>
#include <Graphics/Pipeline/RenderPassContext.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>

namespace fbzz::renderer {

void ExecuteMotionBlurPass(RenderPassContext& ctx)
{
    auto& r       = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h       = ctx.handles;
    const auto& mb = ctx.settings.motionBlur;

    if (!mb.enabled ||
        !h.motionBlurShader.IsValid() || !h.motionBlurResult.IsValid())
        return;

    /// @note 深度バッファ(t7)をサンプルしてピクセルのワールド位置を復元し、前フレームの
    /// @note ViewProjection 行列で再投影してモーションベクトルを計算する。ベクトル方向に
    /// @note motionBlurSamples 点を中心対称サンプルして UAV_MOTION_BLUR(u5) に書く。
    renderer::ComputeCall mbDC;
    mbDC.shader             = h.motionBlurShader;
    /// @note b0: CameraConstants (invViewProjection)
    mbDC.constantBuffers[0] = h.frameCB;
    /// @note b8: motionBlurStrength, motionBlurSamples, prevViewProjection
    mbDC.constantBuffers[8] = h.advancedGraphicsCB;
    /// @note t5: 現フレーム HDR カラー
    mbDC.srvInputs[5]       = resources.GetColorTexture(ctx.Res().Target("HDR"), 0);
    /// @note t7: シーン深度からワールド位置を復元して再投影する。Terrain を含む完全な
    /// @note 不透明深度（hdrRT）を読む。GBuffer depth には地形が無く、地形ピクセルの速度が誤って
    /// @note 算出されてモーションブラーが破綻するため、Deferred でも hdrRT を使う。
    mbDC.srvInputs[7]       = resources.GetDepthTexture(ctx.Res().Target("HDR"));
    /// @note t26: モーションベクター。VelocityPass が動かなかったフレームは無効ハンドルのままで、
    /// @note シェーダーは B=0 を読んで深度再投影へフォールバックする。
    if (ctx.Res().Target("Velocity").IsValid())
        mbDC.srvInputs[26]  = resources.GetColorTexture(ctx.Res().Target("Velocity"), 0);
    /// @note u5: UAV_MOTION_BLUR
    mbDC.uavOutputs[5]      = h.motionBlurResult;
    mbDC.dispatchX          = (ctx.width  + 7) / 8;
    mbDC.dispatchY          = (ctx.height + 7) / 8;
    mbDC.dispatchZ          = 1;
    r.Dispatch(mbDC, resources);
}

} /// @note namespace fbzz::renderer
