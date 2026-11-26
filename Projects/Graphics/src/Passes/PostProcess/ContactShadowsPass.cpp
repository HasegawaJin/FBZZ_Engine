/// @file    ContactShadowsPass.cpp
/// @brief   コンタクトシャドウ — スクリーンスペースのビュー空間レイマーチで。
/// @author  Hasegawa Jin
/// @date    2026-06-23

/// @note シャドウマップが届かない小物直下・近傍の接触影を高精度に生成する Compute パス。
/// @note 通常のシャドウマップはテクセルサイズ/キャスケード遷移の解像度限界で小物に張り付く
/// @note 細かい影を正確に表現できないため、スクリーンスペースで深度バッファを直接トレース
/// @note し解像度に依らない鋭い接触影を低コストで追加する。
#include <Graphics/Renderer/Commands/ShaderBindingSlots.hpp>
#include <Graphics/Passes/PostProcess/PostProcessPasses.hpp>
#include <Graphics/Pipeline/Passes/RenderPassContext.hpp>
#include <Graphics/Renderer/Commands/ComputeCall.hpp>

namespace fbzz::renderer {

void ExecuteContactShadowsPass(RenderPassContext& ctx)
{
    auto& r         = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;
    const auto& cs  = ctx.settings.contactShadow;

    if (!cs.enabled ||
        !h.contactShadowShader.IsValid() || !ctx.Res().Texture("ContactShadowResult").IsValid()) {
        return;
    }

    // Forward でもプリパス後は GBuffer に深度がある。描画方式だけで選ぶと未描画の HDR 深度を読む。
    const auto depthTarget = ctx.gbufferDepthReady ? ctx.Res().Target("GBuffer") : ctx.Res().Target("HDR");
    ComputeCall csDC;
    csDC.shader = h.contactShadowShader;
    csDC.constantBuffers[cb::CAMERA] = h.frameCB;
    csDC.constantBuffers[cb::LIGHT] = h.lightCB;
    csDC.constantBuffers[cb::ADVANCED_GRAPHICS] = h.advancedGraphicsCB;
    csDC.srvInputs[srv::DEPTH] = resources.GetDepthTexture(depthTarget);
    csDC.uavOutputs[uav::CONTACT_SHADOW] = ctx.Res().Texture("ContactShadowResult");
    csDC.dispatchX          = (ctx.width  / 2 + 7) / 8;
    csDC.dispatchY          = (ctx.height / 2 + 7) / 8;
    csDC.dispatchZ          = 1;
    r.Dispatch(csDC, resources);
}


std::string_view ContactShadowsPass::Name() const { return "ContactShadows"; }

void ContactShadowsPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    // HDR は申告しない。本体は gbufferDepthReady が false のときだけ HDR の
    // 深度へ落ちるが、このパスが載るのは «GBuffer が揃う» 経路だけ。申告すると
    // 本描画前の HDR へ偽の依存が張られる。
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

} // namespace fbzz::renderer
