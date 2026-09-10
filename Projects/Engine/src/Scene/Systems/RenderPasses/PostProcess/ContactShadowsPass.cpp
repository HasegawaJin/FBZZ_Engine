/// @file    ContactShadowsPass.cpp
/// @brief   コンタクトシャドウ — スクリーンスペースのビュー空間レイマーチで。
/// @author  Hasegawa Jin
/// @date    2026-06-23
///
/// シャドウマップが届かない小物直下・近傍の接触影を高精度に生成する Compute パス。
/// WHY: 通常のシャドウマップは解像度の限界（テクセルサイズ / キャスケード遷移）で
/// 小物に張り付く細かい影を正確に表現できない。
/// スクリーンスペースで深度バッファを直接トレースすることで
/// 解像度に依らない鋭い接触影を低コストで追加する。
#include "PostProcessPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/ComputeCall.hpp>

namespace fbzz::scene {

void ExecuteContactShadowsPass(RenderPassContext& ctx)
{
    auto& r         = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;
    const auto& cs  = ctx.settings.contactShadow;

    if (!cs.enabled ||
        !h.contactShadowShader.IsValid() || !ctx.Res().Texture("ContactShadowResult").IsValid())
        return;

    // WHAT: 深度バッファ(t7)からビュー空間位置を復元し、ライト方向に沿って
    //       contactShadowSteps ステップのレイマーチを行う。
    //       厚み閾値以上の遮蔽を検出したピクセルを暗化させ UAV_CONTACT_SHADOW(u3) に書く。
    //       出力値 = lerp(1.0, 0.0, shadow) → DeferredLighting で遮蔽係数として乗算する。
    renderer::ComputeCall csDC;
    csDC.shader             = h.contactShadowShader;
    csDC.constantBuffers[0] = h.frameCB;            // b0: CameraConstants (projection, invProjection)
    csDC.constantBuffers[3] = h.lightCB;            // b3: LightConstants (lightDir in view space)
    csDC.constantBuffers[8] = h.advancedGraphicsCB; // b8: contactShadowStrength/rayLen/steps/thick
    // t7: Depth。GBuffer の深度が書けているならそちらを読む。
    // WHY isDeferred ではなく gbufferDepthReady か: Forward + GBuffer プリパスでは
    //     isDeferred が false のまま GBuffer の深度だけが揃っている。isDeferred で
    //     判定すると hdrRT を読みに行き、そこはまだ本描画前で空 (接触影が全面に出る)。
    csDC.srvInputs[7]       = resources.GetDepthTexture(
        ctx.gbufferDepthReady ? ctx.Res().Target("GBuffer") : ctx.Res().Target("HDR"));
    csDC.uavOutputs[3]      = ctx.Res().Texture("ContactShadowResult"); // u3: UAV_CONTACT_SHADOW (UAV_SSR スロットを時分割で再利用)
    csDC.dispatchX          = (ctx.width  / 2 + 7) / 8;
    csDC.dispatchY          = (ctx.height / 2 + 7) / 8;
    csDC.dispatchZ          = 1;
    r.Dispatch(csDC, resources);
}

} // namespace fbzz::scene
