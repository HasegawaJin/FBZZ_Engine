// FBZZ Engine
// RenderPasses/CausticsPass.cpp | fbzz::scene
// 水中コースティクスを HDR バッファへ加算合成するポストプロセスパス
#include "../PostProcess/PostProcessPasses.hpp"
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Renderer/SamplerMode.hpp>
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

        // 複数水面は最も強い設定を代表値として扱う
        if (result.enabled && intensity <= result.intensity) continue;

        result.enabled     = true;
        result.intensity   = intensity;
        result.tiling      = getF("causticsTiling", 4.0f);
        result.surfaceY    = transform.position.y;
        result.timeOffset  = Time::time * getF("causticsSpeed", 0.5f);
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
    ctx.resources.Update(ctx.handles.postprocCB, &postData, sizeof(PostProcCB));

    // WHAT: HDR の色は読まず、深度から復元したワールド座標だけを使って光模様を計算し、加算合成する。
    // WHY: DX11 では同じ HDR RT を SRV と RTV に同時バインドできないため、コピー用 RT を増やさない設計にしている。
    ctx.renderer.SetRenderTarget(ctx.handles.hdrRT, ctx.resources);
    ctx.renderer.SetSampler(0, renderer::SamplerMode::WRAP_ANISOTROPIC);

    renderer::DrawCall dc;
    dc.shader = ctx.handles.causticsShader;
    dc.pipelineState = ctx.handles.causticsPSO;
    dc.vertexCount = 3;
    dc.constantBuffers[0] = ctx.handles.frameCB;
    dc.constantBuffers[5] = ctx.handles.postprocCB;
    dc.textures[0] = causticsTex;
    // WHY: hdrRT を RTV/DSV としてバインドしたまま、その depth を SRV(t7) として読むことは DX11 で禁止。
    //      必要な場合は Water と同様に、描画前の depth を別リソースへコピーしてから t7 に渡す。
    dc.textures[7] = {};
    ctx.renderer.Submit(dc, ctx.resources);
}

} // namespace fbzz::scene
