/// @file    CausticsPass.cpp
/// @brief   水面越しの投影コースティクスを HDR バッファへ加算合成するポストプロセスパス。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include <Graphics/Passes/PostProcess/PostProcessPasses.hpp>
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Pipeline/RenderPassContext.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Math/MathUtils.hpp>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::renderer {

namespace {


renderer::ResourceHandle<renderer::TextureTag> CreateProceduralCaustics(renderer::ResourceManager& resources)
{
    constexpr uint32_t size = 64;
    std::vector<uint8_t> pixels(static_cast<size_t>(size) * static_cast<size_t>(size) * 4u, 255u);

    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(size);
            const float v = static_cast<float>(y) / static_cast<float>(size);
            const float a = std::sin((u * 17.0f + v * 5.0f) * math::TWO_PI);
            const float b = std::sin((u * -7.0f + v * 13.0f) * math::TWO_PI);
            const float c = std::sin((u * 11.0f - v * 19.0f) * math::TWO_PI);
            const float lines = math::Clamp01((a + b + c) * 0.22f + 0.45f);
            const uint8_t value = static_cast<uint8_t>(math::Clamp01(lines * lines) * 255.0f);
            const size_t p = (static_cast<size_t>(y) * size + x) * 4u;
            pixels[p + 0] = value;
            pixels[p + 1] = value;
            pixels[p + 2] = value;
            pixels[p + 3] = 255u;
        }
    }

    return resources.CreateTexture(pixels.data(), size, size);
}

} /// @note namespace

void ExecuteCausticsPass(RenderPassContext& ctx)
{
    if (!ctx.handles.causticsShader.IsValid() || !ctx.handles.causticsPSO.IsValid()) return;

    const auto& source = ctx.environment.caustics;
    if (!source.enabled) return;

    static auto fallbackCaustics = CreateProceduralCaustics(ctx.resources);
    static auto depthCopyShader = ctx.resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DepthCopy.hlsl");
    static uint64_t s_resetVersion = 0;
    /// @note 深度のコピー先はビューが持つ (理由は RenderPassHandles::causticsDepthRT を参照)。
    if (!ctx.handles.causticsDepthRT) return;
    renderer::SizedRenderTarget& s_causticsDepthRT = *ctx.handles.causticsDepthRT;

    if (s_resetVersion != ctx.resources.GetResetVersion()) {
        s_resetVersion = ctx.resources.GetResetVersion();
        fallbackCaustics = CreateProceduralCaustics(ctx.resources);
        depthCopyShader = ctx.resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DepthCopy.hlsl");
    }

    (void)s_causticsDepthRT.Ensure(ctx.resources, ctx.width, ctx.height, renderer::CameraDepthTargetDesc(0));

    renderer::ResourceHandle<renderer::TextureTag> causticsTex = fallbackCaustics;
    if (source.texture.IsValid()) causticsTex = source.texture;

    PostProcCB postData{};
    postData.customParameters[0] = source.intensity;
    postData.customParameters[1] = source.tiling;
    postData.customParameters[2] = source.surfaceY;
    postData.customParameters[3] = source.timeOffset;
    postData.causticsCenterX = source.centerX;
    postData.causticsCenterZ = source.centerZ;
    postData.causticsHalfExtentX = source.halfExtentX;
    postData.causticsHalfExtentZ = source.halfExtentZ;
    postData.causticsWaveAmp = source.waveAmp;
    postData.causticsWaveFreq = source.waveFreq;
    postData.causticsWaveSpeed = source.waveSpeed;
    ctx.resources.Update(ctx.handles.postprocCB, &postData, sizeof(PostProcCB));

    /// @note HDR の depth を専用 RT へコピーし、PS ではコピー後の SRV(t7) からワールド座標を復元する。hdrRT を RTV/DSV として加算先にしながら同じ depth を SRV で読むと DX11 の read/write 競合になる。
    ctx.renderer.SetRenderTarget(s_causticsDepthRT, ctx.resources);
    ctx.renderer.ClearDepth();
    if (depthCopyShader.IsValid()) {
        renderer::DrawCall depthDC;
        depthDC.shader = depthCopyShader;
        depthDC.pipelineState = ctx.handles.defaultPSO;
        depthDC.vertexCount = 3;
        depthDC.textures[7] = ctx.resources.GetDepthTexture(ctx.Res().Target("HDR"));
        ctx.renderer.Submit(depthDC, ctx.resources);
    }

    ctx.renderer.SetRenderTarget(ctx.Res().Target("HDR"), ctx.resources);

    renderer::DrawCall dc;
    dc.shader = ctx.handles.causticsShader;
    dc.pipelineState = ctx.handles.causticsPSO;
    dc.vertexCount = 3;
    dc.constantBuffers[0] = ctx.handles.frameCB;
    dc.constantBuffers[5] = ctx.handles.postprocCB;
    dc.textures[0] = causticsTex;
    dc.textures[7] = ctx.resources.GetDepthTexture(s_causticsDepthRT);
    ctx.renderer.Submit(dc, ctx.resources);
}


void WaterCausticsPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    builder.ReadWrite("HDR");
}

void WaterCausticsPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteCausticsPass(ctx);
}
} /// @note namespace fbzz::renderer
