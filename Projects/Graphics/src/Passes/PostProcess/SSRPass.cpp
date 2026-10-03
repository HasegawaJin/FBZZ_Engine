/// @file    SSRPass.cpp
/// @brief   Screen Space Reflections パス — Compute Shader でレイマーチして映り込みを生成する。
/// @author  Hasegawa Jin
/// @date    2026-06-23

/// @note SSR が使えるのは現在のカメラで見える表面だけ。欠落と交差の不確かさは別 provider へ委ねる。
/// @note GBuffer の法線・深度・金属度が要るため GBuffer 経路でのみ有効。
#include <Graphics/Passes/PostProcess/PostProcessPasses.hpp>
#include <Graphics/Pipeline/RenderPassContext.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>

namespace fbzz::renderer {

void ExecuteSSRPass(RenderPassContext& ctx)
{
    auto& r         = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;
    const auto& ssr = ctx.settings.ssr;
    ctx.ssrPassActive = false;

    /// @note SSR はパイプライン名でなく GBuffer の有無で判定する。Forward 選択時も不透明物は
    /// @note GBuffer 経由になるため、反射の見た目を Deferred と揃えられる。
    if (!ssr.enabled ||
        !resources.Get(h.ssrShader) || !resources.Get(h.ssrResult)
        || !resources.Get(h.frameCB) || !resources.Get(h.advancedGraphicsCB) || !resources.Get(h.postprocCB)
        || !resources.Get(ctx.Res().Target("GBuffer")) || !resources.Get(ctx.Res().Target("HDR")))
        return;

    /// @note 入力: GBuffer0(t0), HDR バッファ(t5), GBuffer1(t6), GBuffer 深度(t7),
    /// @note Forward 不透明物を含む HDR 深度(t25)。視点座標からの反射レイを View Space で
    /// @note トレースして映り込み色を UAV_SSR(u3) に書く。
    renderer::ComputeCall ssrDC;
    ssrDC.shader             = h.ssrShader;
    PostProcCB reflectionData{};
    reflectionData.reflectionResolveEnabled = ctx.hybridReflectionResolveActive ? 1.0f : 0.0f;
    reflectionData.reflectionSsrEnabled = ctx.hybridReflectionSsrPlanned ? 1.0f : 0.0f;
    resources.Update(h.postprocCB, &reflectionData, sizeof(reflectionData));
    ssrDC.constantBuffers[5] = h.postprocCB;
    /// @note b0: CameraConstants
    ssrDC.constantBuffers[0] = h.frameCB;
    /// @note b8: ssrMaxDistance, ssrThickness, ssrSteps, ssrIntensity
    ssrDC.constantBuffers[8] = h.advancedGraphicsCB;
    /// @note t0: GBuffer0 (albedo + roughness)
    ssrDC.srvInputs[0]       = resources.GetColorTexture(ctx.Res().Target("GBuffer"), 0);
    /// @note t5: HDR color (TEX_GBUFFER0 スロット再利用)
    ssrDC.srvInputs[5]       = resources.GetColorTexture(ctx.Res().Target("HDR"), 0);
    /// @note t6: GBuffer1 (normal + metallic)
    ssrDC.srvInputs[6]       = resources.GetColorTexture(ctx.Res().Target("GBuffer"), 1);
    /// @note t7: 反射面の GBuffer 深度
    ssrDC.srvInputs[7]       = resources.GetDepthTexture(ctx.Res().Target("GBuffer"));
    /// @note t25: Forward 不透明物を含むシーン深度
    ssrDC.srvInputs[25]      = resources.GetDepthTexture(ctx.Res().Target("HDR"));
    if (ctx.hybridReflectionResolveActive) {
        ssrDC.srvInputs[22] = resources.GetColorTexture(ctx.Res().Target("GBuffer"), 2);
        if (!resources.Get(ssrDC.srvInputs[22])) return;
    }
    for (const uint32_t slot : {0u, 5u, 6u, 7u, 25u})
        if (!resources.Get(ssrDC.srvInputs[slot])) return;
    /// @note u3: UAV_SSR
    ssrDC.uavOutputs[3]      = h.ssrResult;
    ssrDC.dispatchX          = (ctx.width  + 7) / 8;
    ssrDC.dispatchY          = (ctx.height + 7) / 8;
    ssrDC.dispatchZ          = 1;
    if (r.GetCapabilities().bindless) {
        /// @note DX12 は当該フレームの記録 receipt だけを採用し、Raster 縮退でも旧 SSR を読まない。
        ctx.ssrPassActive = r.TryDispatch(ssrDC, resources);
    } else {
        /// @note DX11 は資源の生存確認後に旧 void Dispatch の契約を維持し、記録成功までは証明しない。
        r.Dispatch(ssrDC, resources);
        ctx.ssrPassActive = true;
    }
}


std::string_view SSRPass::Name() const { return "SSR"; }

void SSRPass::Setup(PassBuilder& builder, const RenderPassContext& ctx) const
{
    builder.Read("GBuffer").Write("SSRResult");
    /// @note Hybrid は source を変更しない。旧 Raster の順序依存だけ ReadWrite 宣言で維持する。
    if (ctx.hybridReflectionResolveActive) builder.Read("HDR");
    else builder.ReadWrite("HDR");
}

bool SSRPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.ssr.enabled
        && (!ctx.hybridReflectionResolveActive || ctx.hybridReflectionSsrPlanned);
}

void SSRPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteSSRPass(ctx);
}

} /// @note namespace fbzz::renderer
