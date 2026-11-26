/// @file    SSRPass.cpp
/// @brief   Screen Space Reflections パス — Compute Shader でレイマーチして映り込みを生成する。
/// @author  Hasegawa Jin
/// @date    2026-06-23

/// @note SSR が使えるのは現在のカメラで見える表面だけ。欠落と交差の不確かさは別 provider へ委ねる。
/// @note GBuffer の法線・深度・金属度が要るため GBuffer 経路でのみ有効。
#include <Graphics/Renderer/Commands/ShaderBindingSlots.hpp>
#include <Graphics/Passes/PostProcess/PostProcessPasses.hpp>
#include <Graphics/Pipeline/Passes/RenderPassContext.hpp>
#include <Graphics/Renderer/Commands/ComputeCall.hpp>
#include <Graphics/Renderer/Commands/DrawCall.hpp>
#include "../../Renderer/Commands/QueuedComputeComposite.hpp"
#include <cstdlib>

namespace fbzz::renderer {
namespace {
/// @note Hybrid の反射選択と TAA の履歴は当該フレームの同期記録を必要とする。
bool QueueRasterComposite(const RenderPassContext& ctx)
{
    // 同一バイナリーで同期経路と比較できるよう、起動時の設定で切り替える。
    static const bool forceSynchronous = [] {
        const char* value = std::getenv("FBZZ_RASTER_SSR_FORCE_SYNC");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return !forceSynchronous && ctx.renderer.GetCapabilities().bindless && !ctx.hybridReflectionResolveActive
        && !ctx.settings.taa.enabled && ctx.settings.passOverrides.empty();
}

DrawCall CompositeDraw(const RenderPassContext& ctx)
{
    DrawCall draw;
    draw.shader = ctx.handles.ssrResolveShader;
    draw.pipelineState = ctx.handles.causticsPSO;
    draw.vertexCount = 3;
    draw.constantBuffers[cb::ADVANCED_GRAPHICS] = ctx.handles.advancedGraphicsCB;
    draw.textures[srv::SSR] = ctx.handles.ssrResult;
    return draw;
}
}

void ExecuteSSRPass(RenderPassContext& ctx)
{
    auto& r         = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;
    const auto& ssr = ctx.settings.ssr;
    ctx.ssrDispatchRecorded = false;
    ctx.ssrContributionSubmitted = false;

    // SSR はパイプライン名でなく GBuffer の有無で判定する。Forward 選択時も不透明物は
    // GBuffer 経由になるため、反射の見た目を Deferred と揃えられる。
    if (!ssr.enabled ||
        !resources.Get(h.ssrShader) || !resources.Get(h.ssrResult)
        || !resources.Get(h.frameCB) || !resources.Get(h.advancedGraphicsCB) || !resources.Get(h.postprocCB)
        || !resources.Get(ctx.Res().Target("GBuffer")) || !resources.Get(ctx.Res().Target("HDR"))) {
        return;
    }

    // 入力: GBuffer0(t0), HDR バッファ(t5), GBuffer1(t6), GBuffer 深度(t7),
    // Forward 不透明物を含む HDR 深度(t25)。視点座標からの反射レイを View Space で
    // トレースして映り込み色を UAV_SSR(u3) に書く。
    renderer::ComputeCall ssrDC;
    ssrDC.shader             = h.ssrShader;
    PostProcCB reflectionData{};
    reflectionData.reflectionResolveEnabled = ctx.hybridReflectionResolveActive ? 1.0f : 0.0f;
    resources.Update(h.postprocCB, &reflectionData, sizeof(reflectionData));
    ssrDC.constantBuffers[cb::POSTPROC] = h.postprocCB;
    ssrDC.constantBuffers[cb::CAMERA] = h.frameCB;
    ssrDC.constantBuffers[cb::ADVANCED_GRAPHICS] = h.advancedGraphicsCB;
    ssrDC.srvInputs[srv::SSR_GBUFFER0]       = resources.GetColorTexture(ctx.Res().Target("GBuffer"), 0);
    // SSR では TEX_GBUFFER0 スロット (t5) を照明済み HDR 色の入力に使う。
    ssrDC.srvInputs[srv::POST_COLOR]       = resources.GetColorTexture(ctx.Res().Target("HDR"), 0);
    ssrDC.srvInputs[srv::GBUFFER1]       = resources.GetColorTexture(ctx.Res().Target("GBuffer"), 1);
    ssrDC.srvInputs[srv::DEPTH]       = resources.GetDepthTexture(ctx.Res().Target("GBuffer"));
    ssrDC.srvInputs[srv::SCENE_DEPTH]      = resources.GetDepthTexture(ctx.Res().Target("HDR"));
    if (ctx.hybridReflectionResolveActive) {
        ssrDC.srvInputs[srv::GBUFFER_EMISSION] = resources.GetColorTexture(ctx.Res().Target("GBuffer"), 2);
        if (!resources.Get(ssrDC.srvInputs[srv::GBUFFER_EMISSION])) {
            return;
        }
    } else {
        ssrDC.srvInputs[srv::IBL_IRRADIANCE] = h.iblIrradiance;
        ssrDC.srvInputs[srv::IBL_PREFILTER] = h.iblPrefilter;
        ssrDC.srvInputs[srv::IBL_BRDF_LUT] = h.iblBrdfLut;
        ssrDC.srvInputs[srv::LIGHT_PROBE_SH_OUTER] = h.lightProbeSH[1];
        ssrDC.srvInputs[srv::LIGHT_PROBE_SH] = h.lightProbeSH[0];
        for (const auto texture : h.lightProbeVisibility) {
            if (texture.IsValid()) {
                ssrDC.indirectReadTextures.push_back(texture);
            }
        }
        // 水中の環境鏡面を差し引くときも、本描画と同じ媒質と光路を評価する。
        if (ctx.waterMediumPassActive && h.waterMediumTable.IsValid() && h.waterMediumAtlasRT
            && h.waterMediumAtlasRT->IsValid()) {
            ssrDC.srvBuffers[srv::WATER_MEDIUM_TABLE] = h.waterMediumTable;
            ssrDC.srvInputs[srv::WATER_MEDIUM_ATLAS] = resources.GetColorTexture(*h.waterMediumAtlasRT, 0);
        }
    }
    for (const uint32_t slot : {srv::SSR_GBUFFER0, srv::POST_COLOR, srv::GBUFFER1, srv::DEPTH, srv::SCENE_DEPTH}) {
        if (!resources.Get(ssrDC.srvInputs[slot])) {
            return;
        }
    }
    ssrDC.uavOutputs[uav::SSR]      = h.ssrResult;
    ssrDC.dispatchX          = (ctx.width  + 7) / 8;
    ssrDC.dispatchY          = (ctx.height + 7) / 8;
    ssrDC.dispatchZ          = 1;
    if (QueueRasterComposite(ctx)) {
        if (!resources.Get(h.ssrResolveShader) || !resources.Get(h.causticsPSO)) {
            return;
        }
        // キュー内の合成は記録に成功した場合だけ行い、CPU へ成功状態を公開しない。
        compute_composite_detail::Submit(r, resources, ssrDC, ctx.Res().Target("HDR"), CompositeDraw(ctx));
        return;
    }
    ctx.ssrDispatchRecorded = r.TryDispatch(ssrDC, resources);
}


std::string_view SSRPass::Name() const { return "SSR"; }

void SSRPass::Setup(PassBuilder& builder, const RenderPassContext& ctx) const
{
    builder.Read("GBuffer");
    if (QueueRasterComposite(ctx)) {
        builder.ReadWrite("SSRResult").ReadWrite("HDR");
    }
    else {
        builder.Write("SSRResult").Read("HDR");
    }
    if (!ctx.hybridReflectionResolveActive && ctx.waterMediumPassActive) {
        builder.Read("WaterMediumAtlas");
    }
}

bool SSRPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.ssrPassPlanned;
}

void SSRPass::Execute(PassResources& resources, RenderPassContext& ctx)
{
    if (resources.Texture("SSRResult") != ctx.handles.ssrResult) {
        return;
    }
    ExecuteSSRPass(ctx);
}

std::string_view SSRResolvePass::Name() const { return "SSRResolve"; }
void SSRResolvePass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    builder.Read("SSRResult").ReadWrite("HDR");
}
bool SSRResolvePass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.ssrPassPlanned && !ctx.hybridReflectionResolveActive && !QueueRasterComposite(ctx);
}
void SSRResolvePass::Execute(PassResources& resources, RenderPassContext& ctx)
{
    ctx.ssrContributionSubmitted = false;
    auto& h = ctx.handles;
    const auto hdr = resources.Target("HDR");
    if (!ctx.ssrDispatchRecorded || resources.Texture("SSRResult") != h.ssrResult
        || !ctx.resources.Get(h.ssrResult) || !ctx.resources.Get(h.advancedGraphicsCB)
        || !ctx.resources.Get(h.ssrResolveShader) || !ctx.resources.Get(h.causticsPSO)
        || !ctx.resources.Get(hdr)) {
        return;
    }
    ctx.renderer.SetRenderTarget(hdr, ctx.resources);
    ctx.renderer.Submit(CompositeDraw(ctx), ctx.resources);
    ctx.ssrContributionSubmitted = true;
}

}
