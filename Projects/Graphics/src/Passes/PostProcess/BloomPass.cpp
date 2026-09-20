/// @file    BloomPass.cpp
/// @brief   Bloom — ミップ連鎖でダウンサンプルし、逆順に足し戻して広いにじみを作る
/// @author  Hasegawa Jin
/// @date    2026-08-25

/// @note HDR を閾値+縮小しながら chain0..4 へダウンサンプル (kBloomMipCount=5)、逆順に
/// @note 拡大+加算して chain0 へ戻し bloomFull へ拡大・上書きする。段を積むのは、1 段だけ
/// @note だとぼけ半径が全解像度で ±4px 程度しかなく、光らせるには発光を強くして白クリップ
/// @note させるしかないため。段を積み面積でにじませると彩度を保ったまま光って見せられる。
#include <Graphics/Passes/PostProcess/PostProcessPasses.hpp>
#include <Graphics/Pipeline/RenderPassContext.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>
#include <algorithm>

namespace fbzz::renderer {

namespace {

/// @note 1 段ぶんのディスパッチ。src を読んで dst へ書く。
/// @note add が有効なら、dst と同じ寸法のテクスチャを t9 から読んで足す
/// @note (dst を読むと RGBA16F の typed UAV load になり D3D11 で未定義動作)。
void BloomStep(RenderPassContext& ctx,
               renderer::ResourceHandle<renderer::ShaderTag> shader,
               renderer::ResourceHandle<renderer::TextureTag> src,
               uint32_t srcW, uint32_t srcH,
               renderer::ResourceHandle<renderer::TextureTag> dst,
               uint32_t dstW, uint32_t dstH,
               bool applyThreshold,
               renderer::ResourceHandle<renderer::TextureTag> add = {})
{
    const bool additive = add.IsValid();
    const auto& rs = ctx.settings;

    /// @note texelSize = 書き込み先
    PostProcCB data = MakeScreenPostProcCB(dstW, dstH);
    data.screenSize[0] = static_cast<float>(ctx.width);
    data.screenSize[1] = static_cast<float>(ctx.height);
    data.bloomSrcTexel[0] = 1.0f / static_cast<float>((std::max)(1u, srcW));
    data.bloomSrcTexel[1] = 1.0f / static_cast<float>((std::max)(1u, srcH));
    data.bloomThreshold   = rs.postProcess.bloom.threshold;
    data.bloomSoftKnee    = rs.postProcess.bloom.softKnee;
    /// @note BloomDownsample は bloomIntensity <= 0 を「無効」と見なして出力をゼロ埋めする。
    /// @note 合成側の強度は Composite が別途 b5 に載せるため、ここでは有効フラグとしてのみ使う。
    data.bloomIntensity      = rs.postProcess.bloom.intensity;
    data.bloomApplyThreshold = applyThreshold ? 1.0f : 0.0f;
    data.bloomAdditive       = additive ? 1.0f : 0.0f;
    ctx.resources.Update(ctx.handles.postprocCB, &data, sizeof(PostProcCB));

    renderer::ComputeCall dc;
    dc.shader = shader;
    dc.constantBuffers[5] = ctx.handles.postprocCB;
    dc.srvInputs[10]      = src;
    /// @note t9 = TEX_BLOOM_ADD
    if (additive) dc.srvInputs[9] = add;
    dc.uavOutputs[0]      = dst;
    dc.dispatchX = (dstW + 7) / 8;
    dc.dispatchY = (dstH + 7) / 8;
    dc.dispatchZ = 1;
    ctx.renderer.Dispatch(dc, ctx.resources);
}

} /// @note namespace

void ExecuteBloomPass(RenderPassContext& ctx)
{
    auto& h = ctx.handles;
    const auto& rs = ctx.settings;

    if (!rs.postProcess.bloom.enabled
        || !h.bloomDownShader.IsValid() || !h.bloomUpShader.IsValid()
        || !ctx.Res().Texture("Bloom").IsValid())
        return;

    /// @note 連鎖のうち実際に使える段数。解像度が小さいと下の段が 1px に潰れるので、
    /// @note 潰れた段は積まない (同じ寸法へ縮小し続けても情報が増えず、無駄なだけ)。
    uint32_t mips = 0;
    for (uint32_t i = 0; i < kBloomMipCount; ++i) {
        if (!h.bloomChain[i].IsValid() || !h.bloomUpChain[i].IsValid()) break;
        if (i > 0 && h.bloomChainWidth[i] <= 1 && h.bloomChainHeight[i] <= 1) break;
        ++mips;
    }
    if (mips == 0) return;

    /// @name 1. ダウンサンプル
    /// @note 1 段目だけ HDR から読み、輝度閾値で「何を光らせるか」を選別する。
    BloomStep(ctx, h.bloomDownShader,
              ctx.resources.GetColorTexture(ctx.Res().Target("HDR"), 0), ctx.width, ctx.height,
              h.bloomChain[0], h.bloomChainWidth[0], h.bloomChainHeight[0],
              /*applyThreshold=*/true);

    for (uint32_t i = 1; i < mips; ++i) {
        BloomStep(ctx, h.bloomDownShader,
                  h.bloomChain[i - 1], h.bloomChainWidth[i - 1], h.bloomChainHeight[i - 1],
                  h.bloomChain[i],     h.bloomChainWidth[i],     h.bloomChainHeight[i],
                  false);
    }

    /// @name 2. アップサンプルして足し戻す
    /// @note up[mips-1] = chain[mips-1]              (最小段はぼかす相手がいない)
    /// @note up[i]      = tent(up[i+1]) + chain[i]
    /// @note 読むのは up[i+1] と chain[i]、書くのは up[i] で、すべて別のテクスチャ。
    uint32_t srcIdx = mips - 1;
    /// @note 最小段はダウンサンプル結果そのもの
    auto     srcTex = h.bloomChain[mips - 1];
    for (int i = static_cast<int>(mips) - 2; i >= 0; --i) {
        const uint32_t u = static_cast<uint32_t>(i);
        BloomStep(ctx, h.bloomUpShader,
                  srcTex,             h.bloomChainWidth[srcIdx], h.bloomChainHeight[srcIdx],
                  h.bloomUpChain[u],  h.bloomChainWidth[u],      h.bloomChainHeight[u],
                  false, /*add=*/h.bloomChain[u]);
        srcTex = h.bloomUpChain[u];
        srcIdx = u;
    }

    /// @name 3. 全解像度へ
    /// @note ここは足さずにそのまま書く。bloomFull は前フレームの内容を持っているので、
    /// @note 加算にすると毎フレーム明るさが積み上がって発散する。
    BloomStep(ctx, h.bloomUpShader,
              srcTex,      h.bloomChainWidth[srcIdx], h.bloomChainHeight[srcIdx],
              ctx.Res().Texture("Bloom"), ctx.width,                 ctx.height,
              false);
}


void BloomPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    builder.Read("HDR").Write("Bloom");
}

bool BloomPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.postProcess.bloom.enabled;
}

void BloomPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteBloomPass(ctx);
}
} /// @note namespace fbzz::renderer
