/// @file    CausticsPass.cpp
/// @brief   水面越しの投影コースティクスを HDR バッファへ加算合成するポストプロセスパス。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include <Graphics/Passes/PostProcess/PostProcessPasses.hpp>
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Pipeline/RenderPassContext.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Renderer/TextureFileDecoder.hpp>
#include <Math/MathUtils.hpp>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::renderer {

namespace {


renderer::ResourceHandle<renderer::TextureTag> CreateProceduralCaustics(renderer::ResourceManager& resources)
{
    constexpr uint32_t size = 256;
    std::vector<uint8_t> pixels(static_cast<size_t>(size) * static_cast<size_t>(size) * 4u, 255u);

    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(size);
            const float v = static_cast<float>(y) / static_cast<float>(size);
            /// @note A periodic cellular ridge mask concentrates additive light into thin connected lines rather than broad moving bands.
            /// @see https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-2-rendering-water-caustics GPU Gems, additive projected caustic textures.
            constexpr int CELL_COUNT = 6;
            const float gridX = u * static_cast<float>(CELL_COUNT)
                + 0.3f * std::sin((2.0f * u + v) * math::TWO_PI)
                + 0.15f * std::sin((3.0f * v - u) * math::TWO_PI);
            const float gridY = v * static_cast<float>(CELL_COUNT)
                + 0.3f * std::sin((2.0f * v - u) * math::TWO_PI)
                + 0.15f * std::cos((3.0f * u + v) * math::TWO_PI);
            const int originX = static_cast<int>(std::floor(gridX));
            const int originY = static_cast<int>(std::floor(gridY));
            float first = 100.0f, second = 100.0f;
            for (int offsetY = -1; offsetY <= 1; ++offsetY) {
                for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                    const int cellX = originX + offsetX;
                    const int cellY = originY + offsetY;
                    const uint32_t wrappedX = static_cast<uint32_t>((cellX + CELL_COUNT) % CELL_COUNT);
                    const uint32_t wrappedY = static_cast<uint32_t>((cellY + CELL_COUNT) % CELL_COUNT);
                    uint32_t hash = wrappedX * 1597334677u ^ wrappedY * 3812015801u ^ 0x9E3779B9u;
                    hash ^= hash >> 16u;
                    hash *= 2246822519u;
                    hash ^= hash >> 13u;
                    const float jitterX = 0.2f + 0.6f * static_cast<float>(hash & 65535u) / 65535.0f;
                    const float jitterY = 0.2f + 0.6f * static_cast<float>(hash >> 16u) / 65535.0f;
                    const float dx = static_cast<float>(cellX) + jitterX - gridX;
                    const float dy = static_cast<float>(cellY) + jitterY - gridY;
                    const float distance = dx * dx + dy * dy;
                    if (distance < first) { second = first; first = distance; }
                    else if (distance < second) second = distance;
                }
            }
            const float edgeDistance = std::sqrt(second) - std::sqrt(first);
            const float line = std::exp(-edgeDistance * 22.0f);
            const uint8_t value = static_cast<uint8_t>(math::Clamp01(line) * 255.0f);
            const size_t p = (static_cast<size_t>(y) * size + x) * 4u;
            pixels[p + 0] = value;
            pixels[p + 1] = value;
            pixels[p + 2] = value;
            pixels[p + 3] = 255u;
        }
    }

    /// @note Minified caustic lines need a complete mip pyramid to avoid moire on distant and oblique receivers.
    /// @see https://www.pbr-book.org/4ed/Textures_and_Materials/Image_Texture PBRT v4, Image Texture Evaluation.
    DecodedTextureRGBA8 texture;
    texture.sourceWidth = texture.sourceHeight = size;
    texture.mips.push_back({size, size, std::move(pixels)});
    while (texture.mips.back().width > 1u || texture.mips.back().height > 1u)
        texture.mips.push_back(DownsampleRGBA8Box2x(texture.mips.back()));
    std::vector<TextureMipData> mips;
    if (!texture.ToMipData(mips)) return {};
    return resources.CreateTextureWithMips(mips.data(), static_cast<uint32_t>(mips.size()));
}

} /// @note namespace

void ExecuteCausticsPass(RenderPassContext& ctx)
{
    if (!ctx.handles.causticsShader.IsValid() || !ctx.handles.causticsPSO.IsValid()) return;

    const auto& source = ctx.environment.caustics;
    bool hasMediumCaustics = false;
    if (ctx.waterMediumPassActive && ctx.renderScene) {
        for (const auto& body : ctx.handles.waterMediumBodies) {
            if (body.waterInputIndex < ctx.renderScene->water.size()
                && ctx.renderScene->water[body.waterInputIndex].medium.causticsEnabled) {
                hasMediumCaustics = true;
                break;
            }
        }
    }
    if (!source.enabled && !hasMediumCaustics) return;

    static ResourceHandle<TextureTag> fallbackCaustics;
    static ResourceHandle<ShaderTag> depthCopyShader;
    static uint64_t s_resourceOwner = 0;
    static uint64_t s_resetVersion = 0;
    /// @note 深度のコピー先はビューが持つ (理由は RenderPassHandles::causticsDepthRT を参照)。
    if (!ctx.handles.causticsDepthRT) return;
    renderer::SizedRenderTarget& s_causticsDepthRT = *ctx.handles.causticsDepthRT;

    if (s_resourceOwner != ctx.resources.GetInstanceId() || s_resetVersion != ctx.resources.GetResetVersion()) {
        s_resourceOwner = ctx.resources.GetInstanceId();
        s_resetVersion = ctx.resources.GetResetVersion();
        fallbackCaustics = CreateProceduralCaustics(ctx.resources);
        depthCopyShader = ctx.resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DepthCopy.hlsl");
    }
    if (!depthCopyShader.IsValid() || (!source.texture.IsValid() && !fallbackCaustics.IsValid())) return;

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
    postData.time = ctx.time;
    postData.underwaterOpticalMode = source.opticalEnabled ? 1.0f : 0.0f;
    const auto absorption = source.opticalEnabled ? source.absorption : math::Vector3{0.45f, 0.45f, 0.45f};
    const auto scattering = source.opticalEnabled ? source.scattering : math::Vector3{};
    postData.underwaterAbsorption[0] = absorption.x;
    postData.underwaterAbsorption[1] = absorption.y;
    postData.underwaterAbsorption[2] = absorption.z;
    postData.underwaterScattering[0] = scattering.x;
    postData.underwaterScattering[1] = scattering.y;
    postData.underwaterScattering[2] = scattering.z;
    postData.underwaterPhaseG = source.phaseG;
    postData.underwaterSurfaceY = source.surfaceY;
    postData.underwaterBottomY = -1000000.0f;
    postData.underwaterMaxDistance = source.maxDistance;
    postData.underwaterCenter[0] = source.centerX;
    postData.underwaterCenter[1] = source.centerZ;
    postData.underwaterHalfExtent[0] = source.halfExtentX;
    postData.underwaterHalfExtent[1] = source.halfExtentZ;
    postData.underwaterCosYaw = source.cosYaw;
    postData.underwaterSinYaw = source.sinYaw;
    postData.underwaterWaveAmplitude = source.waveAmp;
    postData.underwaterWaveFrequency = source.waveFreq;
    postData.underwaterWaveSpeed = source.waveSpeed;
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
    dc.constantBuffers[3] = ctx.handles.lightCB;
    dc.constantBuffers[4] = ctx.handles.shadowCB;
    dc.constantBuffers[5] = ctx.handles.postprocCB;
    dc.constantBuffers[8] = ctx.handles.advancedGraphicsCB;
    if (ctx.waterMediumPassActive && ctx.handles.waterMediumTable.IsValid()
        && ctx.handles.waterMediumAtlasRT && ctx.handles.waterMediumAtlasRT->IsValid()) {
        dc.auxiliaryPsBuffers[0] = ctx.handles.waterMediumTable;
        dc.textures[15] = ctx.resources.GetColorTexture(*ctx.handles.waterMediumAtlasRT, 0);
    }
    if (ctx.waterInterfacePassActive && ctx.handles.waterInterfaceRT
        && ctx.handles.waterInterfaceRT->IsValid())
        dc.textures[13] = ctx.resources.GetColorTexture(*ctx.handles.waterInterfaceRT, 0);
    dc.textures[0] = causticsTex;
    dc.textures[7] = ctx.resources.GetDepthTexture(s_causticsDepthRT);
    dc.textures[8] = ctx.resources.GetDepthTexture(ctx.Res().Target("ShadowMap"));
    ctx.renderer.Submit(dc, ctx.resources);
}


void WaterCausticsPass::Setup(PassBuilder& builder, const RenderPassContext& ctx) const
{
    builder.ReadWrite("HDR").Read("ShadowMap");
    if (ctx.waterMediumPassActive)
        builder.Read("WaterMediumAtlas");
    if (ctx.waterInterfacePassActive)
        builder.Read("WaterInterface");
}

void WaterCausticsPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteCausticsPass(ctx);
}
} /// @note namespace fbzz::renderer
