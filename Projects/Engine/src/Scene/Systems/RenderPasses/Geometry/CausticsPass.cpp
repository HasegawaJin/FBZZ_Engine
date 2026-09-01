/// @file    RenderPasses/CausticsPass.cpp
/// @brief   水面越しの投影コースティクスを HDR バッファへ加算合成するポストプロセスパス。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include "../PostProcess/PostProcessPasses.hpp"
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Math/MathUtils.hpp>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::scene {

namespace {

struct CausticsSource {
    bool enabled = false;
    float intensity = 0.0f;
    float tiling = 1.0f;
    float surfaceY = 0.0f;
    float timeOffset = 0.0f;
    float centerX = 0.0f;
    float centerZ = 0.0f;
    float halfExtentX = 0.0f;
    float halfExtentZ = 0.0f;
    float waveAmp = 0.0f;
    float waveFreq = 0.12f;
    float waveSpeed = 1.0f;
    std::string texturePath;
};

CausticsSource FindCausticsSource(RenderPassContext& ctx)
{
    CausticsSource result{};
    for (auto [water, transform] : ctx.scene.View<WaterComponent, Transform>()) {
        if (!water.enabled) continue;

        const asset::MaterialAsset* mat = nullptr;
        if (!water.materialPath.empty()) {
            const auto handle = asset::AssetManager::LoadMaterial(water.materialPath);
            mat = asset::AssetManager::GetMaterial(handle);
        }

        auto getF = [mat](const char* name, float def) -> float {
            if (!mat) return def;
            const auto it = mat->params.find(name);
            if (it != mat->params.end() && !it->second.empty()) return it->second[0];
            return def;
        };
        auto getTex = [mat](const char* name) -> std::string {
            if (!mat) return {};
            const auto it = mat->textures.find(name);
            if (it != mat->textures.end()) return it->second;
            return {};
        };

        const float enableCaustics  = getF("enableCaustics", 0.0f);
        if (enableCaustics < 0.5f) continue;

        const float intensity = getF("causticsIntensity", 1.0f);
        if (intensity <= 0.0f) continue;

        float totalAmp = 0.0f;
        float weightedWaveFreq = 0.0f;
        for (const auto& wave : water.waves) {
            if (wave.amplitude <= 0.0f || wave.wavelength <= math::EPSILON) continue;
            totalAmp += wave.amplitude;
            weightedWaveFreq += (math::TWO_PI / wave.wavelength) * wave.amplitude;
        }

        // 複数水面は最も強い設定を代表値として扱う。
        // WHY: 1 回のフルスクリーン加算で済ませるため、Phase C-2 では代表水面のみを投影元にする。
        if (result.enabled && intensity <= result.intensity) continue;

        result.enabled     = true;
        result.intensity   = intensity;
        result.tiling      = getF("causticsTiling", 4.0f);
        result.surfaceY    = transform.position.y;
        result.timeOffset  = Time::time * getF("causticsSpeed", 0.5f);
        result.centerX     = transform.position.x;
        result.centerZ     = transform.position.z;
        result.halfExtentX = water.extentX * 0.5f;
        result.halfExtentZ = water.extentZ * 0.5f;
        result.waveAmp     = getF("causticsWaveAmp", math::Clamp(totalAmp * 0.12f, 0.015f, 0.18f));
        result.waveFreq    = getF("causticsWaveFreq", totalAmp > math::EPSILON ? weightedWaveFreq / totalAmp : 0.12f);
        result.waveSpeed   = getF("causticsWaveSpeed", 1.0f);
        result.texturePath = getTex("causticsTex");
    }
    return result;
}

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

} // anonymous namespace

void ExecuteCausticsPass(RenderPassContext& ctx)
{
    if (!ctx.handles.causticsShader.IsValid() || !ctx.handles.causticsPSO.IsValid()) return;

    const CausticsSource source = FindCausticsSource(ctx);
    if (!source.enabled) return;

    static auto fallbackCaustics = CreateProceduralCaustics(ctx.resources);
    static std::string s_loadedPath;
    static renderer::ResourceHandle<renderer::TextureTag> s_loadedTexture;
    static auto depthCopyShader = ctx.resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DepthCopy.hlsl");
    static renderer::SizedRenderTarget s_causticsDepthRT;
    static uint64_t s_resetVersion = 0;

    if (s_resetVersion != ctx.resources.GetResetVersion()) {
        s_resetVersion = ctx.resources.GetResetVersion();
        fallbackCaustics = CreateProceduralCaustics(ctx.resources);
        if (!source.texturePath.empty()) {
            s_loadedPath.clear();
            s_loadedTexture = {};
        }
        depthCopyShader = ctx.resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DepthCopy.hlsl");
    }

    (void)s_causticsDepthRT.Ensure(ctx.resources, ctx.width, ctx.height, 0);

    renderer::ResourceHandle<renderer::TextureTag> causticsTex = fallbackCaustics;
    if (!source.texturePath.empty()) {
        if (source.texturePath != s_loadedPath) {
            s_loadedPath = source.texturePath;
            s_loadedTexture = ctx.resources.LoadTexture(source.texturePath);
        }
        if (s_loadedTexture.IsValid()) {
            causticsTex = s_loadedTexture;
        }
    }

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

    // WHAT: HDR の depth を専用 RT へコピーし、PS ではコピー後の SRV からワールド座標を復元する。
    // WHY: hdrRT を RTV/DSV として加算先にしながら同じ depth を SRV(t7) で読むと DX11 の read/write 競合になる。
    ctx.renderer.SetRenderTarget(s_causticsDepthRT, ctx.resources);
    ctx.renderer.ClearDepth();
    if (depthCopyShader.IsValid()) {
        renderer::DrawCall depthDC;
        depthDC.shader = depthCopyShader;
        depthDC.pipelineState = ctx.handles.defaultPSO;
        depthDC.vertexCount = 3;
        depthDC.textures[7] = ctx.resources.GetDepthTexture(ctx.handles.hdrRT);
        ctx.renderer.Submit(depthDC, ctx.resources);
    }

    ctx.renderer.SetRenderTarget(ctx.handles.hdrRT, ctx.resources);

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

} // namespace fbzz::scene
