/// @file    ContactShadowsPass.cpp
/// @brief   コンタクトシャドウ — スクリーンスペースのビュー空間レイマーチで。
/// @author  Hasegawa Jin
/// @date    2026-06-23
///
/// @note シャドウマップが届かない小物直下・近傍の接触影を高精度に生成する Compute パス。
///       通常のシャドウマップはテクセルサイズ/キャスケード遷移の解像度限界で小物に張り付く
///       細かい影を正確に表現できないため、スクリーンスペースで深度バッファを直接トレース
///       し解像度に依らない鋭い接触影を低コストで追加する。
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

    /// @note 深度バッファ(t7)からビュー空間位置を復元し、ライト方向に沿って
    ///       contactShadowSteps ステップのレイマーチを行う。厚み閾値以上の遮蔽を検出した
    ///       ピクセルを暗化させ UAV_CONTACT_SHADOW(u3) に書く。出力値は lerp(1.0, 0.0, shadow)
    ///       で DeferredLighting が遮蔽係数として乗算する。
    renderer::ComputeCall csDC;
    csDC.shader             = h.contactShadowShader;
    /// @note b0: CameraConstants (projection, invProjection)
    csDC.constantBuffers[0] = h.frameCB;
    /// @note b3: LightConstants (lightDir はワールド空間。シェーダーが view でビュー空間へ回す)
    csDC.constantBuffers[3] = h.lightCB;
    /// @note b8: contactShadowStrength/rayLen/steps/thick
    csDC.constantBuffers[8] = h.advancedGraphicsCB;
    /// @note t7: Depth。GBuffer の深度が書けているならそちらを読む。isDeferred ではなく
    ///       gbufferDepthReady で判定するのは、Forward + GBuffer プリパスでは isDeferred が
    ///       false のまま GBuffer の深度だけ揃っているため。isDeferred で判定すると hdrRT を
    ///       読みに行き、本描画前で空のままになり接触影が全面に出る。
    csDC.srvInputs[7]       = resources.GetDepthTexture(
        ctx.gbufferDepthReady ? ctx.Res().Target("GBuffer") : ctx.Res().Target("HDR"));
    /// @note u3: UAV_CONTACT_SHADOW (UAV_SSR スロットを時分割で再利用)
    csDC.uavOutputs[3]      = ctx.Res().Texture("ContactShadowResult");
    csDC.dispatchX          = (ctx.width  / 2 + 7) / 8;
    csDC.dispatchY          = (ctx.height / 2 + 7) / 8;
    csDC.dispatchZ          = 1;
    r.Dispatch(csDC, resources);
}


std::string_view ContactShadowsPass::Name() const { return "ContactShadows"; }

void ContactShadowsPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    /// @note HDR は申告しない。本体は gbufferDepthReady が false のときだけ HDR の
    ///       深度へ落ちるが、このパスが載るのは «GBuffer が揃う» 経路だけ。申告すると
    ///       本描画前の HDR へ偽の依存が張られる。
    builder.Read("GBuffer").Write("ContactShadowResult");
}

bool ContactShadowsPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.contactShadow.enabled;
}

void ContactShadowsPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteContactShadowsPass(ctx);
}

} // namespace fbzz::scene
