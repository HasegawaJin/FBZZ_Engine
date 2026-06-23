// FBZZ Engine
// CompositePass.cpp | fbzz::scene
// Composite render pass implementation
#include "PostProcessPasses.hpp"
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Renderer/SamplerMode.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Util/Mathf.hpp>
#include <cmath>

namespace fbzz::scene {

namespace {

struct UnderwaterInfo {
    bool enabled = false;
    float strength = 0.0f;
    float depth = 0.0f;
    float fogDensity = 0.0f;
    math::Vector3 color = math::Vector3::ZERO;
};

UnderwaterInfo EvaluateUnderwaterInfo(const RenderPassContext& ctx)
{
    UnderwaterInfo best{};
    const float time = Time::time;

    for (auto [water, transform] : ctx.scene.View<WaterComponent, Transform>()) {
        if (!water.enabled) continue;

        const float localX = ctx.camera.m_position.x - transform.position.x;
        const float localZ = ctx.camera.m_position.z - transform.position.z;
        if (std::abs(localX) > water.extentX * 0.5f || std::abs(localZ) > water.extentZ * 0.5f) {
            continue;
        }

        // WHY: 水面は GPU で揺らすが、カメラ水没判定はポストプロセス前に CPU で決める必要がある。
        //      WaterComponent の Gerstner 評価を再利用し、描画された水面と近い高さで判定する。
        const float surfaceY = transform.position.y + water.GetSurfaceHeightAt(localX, localZ, time);
        const float depth = surfaceY - ctx.camera.m_position.y;
        if (depth <= 0.0f || depth <= best.depth) continue;

        // 視覚パラメータは fzmat から読む。未設定時は WaterComponent のデフォルト値と揃えた定数を使う。
        const asset::MaterialAsset* mat = nullptr;
        if (!water.materialPath.empty()) {
            const auto handle = asset::AssetManager::LoadMaterial(water.materialPath);
            mat = asset::AssetManager::GetMaterial(handle);
        }
        auto getF = [mat](const char* name, float def) -> float {
            if (!mat) return def;
            auto it = mat->params.find(name);
            if (it != mat->params.end() && !it->second.empty()) return it->second[0];
            return def;
        };
        auto getF3 = [mat](const char* name, math::Vector3 def) -> math::Vector3 {
            if (!mat) return def;
            auto it = mat->params.find(name);
            if (it != mat->params.end() && it->second.size() >= 3)
                return { it->second[0], it->second[1], it->second[2] };
            return def;
        };

        const float deepDepth = util::Mathf::Max(getF("deepDepth", 5.0f), 0.001f);
        const float t = util::Mathf::Clamp01(depth / deepDepth);
        const auto shallowColor = getF3("shallowColor", { 0.20f, 0.60f, 0.70f });
        const auto deepColor    = getF3("deepColor",    { 0.00f, 0.10f, 0.30f });
        best.enabled = true;
        best.depth = depth;
        best.strength = util::Mathf::Clamp01(depth / 2.0f);
        best.fogDensity = util::Mathf::Lerp(0.04f, 0.35f, t);
        best.color = {
            util::Mathf::Lerp(shallowColor.x, deepColor.x, t),
            util::Mathf::Lerp(shallowColor.y, deepColor.y, t),
            util::Mathf::Lerp(shallowColor.z, deepColor.z, t)
        };
    }

    return best;
}

} // anonymous namespace

void ExecuteCompositePass(RenderPassContext& ctx)
{
    auto& r = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h = ctx.handles;
    const auto& rs = ctx.settings;

    const auto& pp = rs.postProcess;
    bool hasCustomPostProcess = false;
    for (const auto shader : h.customPostProcessShaders) {
        if (shader.IsValid()) {
            hasCustomPostProcess = true;
            break;
        }
    }
    // WHY: TAA は Composite の出力 (ldrRT) を t5 として読む。
    //      taa.enabled の場合も ldrRT に書かないと TAA が stale なバッファを読んで黒になる。
    //      RenderSystem 側の needsLdrIntermediate と必ず一致させること。
    const bool needsLdrIntermediate =
        pp.fxaaEnabled || ctx.selectionOutlineEnabled ||
        (hasCustomPostProcess && h.customPostProcessRT[0].IsValid()) ||
        rs.IsTaaActive();
    r.SetRenderTarget(needsLdrIntermediate ? h.ldrRT : ctx.outputRT, resources);

    PostProcCB postData{};
    postData.texelSize[0] = 1.0f / static_cast<float>(ctx.width);
    postData.texelSize[1] = 1.0f / static_cast<float>(ctx.height);
    postData.screenSize[0] = static_cast<float>(ctx.width);
    postData.screenSize[1] = static_cast<float>(ctx.height);
    postData.exposure = pp.exposure;
    postData.time = Time::time;
    postData.bloomIntensity = pp.bloom.enabled ? pp.bloom.intensity : 0.0f;
    postData.fogDensity = pp.fog.enabled ? pp.fog.density : 0.0f;
    postData.fogFar = pp.fog.farDistance;
    postData.fogColor[0] = pp.fog.color[0];
    postData.fogColor[1] = pp.fog.color[1];
    postData.fogColor[2] = pp.fog.color[2];
    postData.contrast = pp.colorGrading.enabled ? pp.colorGrading.contrast : 0.0f;
    postData.saturation = pp.colorGrading.enabled ? pp.colorGrading.saturation : 1.0f;
    postData.hueShift = pp.colorGrading.enabled ? pp.colorGrading.hueShift : 0.0f;
    postData.temperature = pp.colorGrading.enabled ? pp.colorGrading.temperature : 0.0f;
    postData.tint = pp.colorGrading.enabled ? pp.colorGrading.tint : 0.0f;
    postData.vignetteIntensity = pp.vignette.enabled ? pp.vignette.intensity : 0.0f;
    postData.vignetteSmoothness = pp.vignette.smoothness;
    postData.vignetteRoundness = pp.vignette.roundness;
    postData.vignetteColor[0] = pp.vignette.color[0];
    postData.vignetteColor[1] = pp.vignette.color[1];
    postData.vignetteColor[2] = pp.vignette.color[2];
    postData.filmGrainIntensity = pp.filmGrain.enabled ? pp.filmGrain.intensity : 0.0f;
    postData.filmGrainResponse = pp.filmGrain.response;
    postData.chromaticAberration = pp.lens.chromaticAberrationEnabled ? pp.lens.chromaticAberration : 0.0f;
    postData.lensDistortion = pp.lens.distortionEnabled ? pp.lens.distortion : 0.0f;
    postData.sharpenStrength = pp.sharpen.enabled ? pp.sharpen.strength : 0.0f;
    postData.sharpenRadius = pp.sharpen.radius;
    postData.dofFocusDistance = pp.depthOfField.focusDistance;
    postData.dofFocusRange = pp.depthOfField.focusRange;
    postData.dofBlurRadius = pp.depthOfField.enabled ? pp.depthOfField.blurRadius : 0.0f;
    postData.sepiaIntensity = pp.stylized.sepiaEnabled ? pp.stylized.sepiaIntensity : 0.0f;
    postData.invertIntensity = pp.stylized.invertEnabled ? pp.stylized.invertIntensity : 0.0f;
    postData.posterizeLevels = pp.stylized.posterizeEnabled ? pp.stylized.posterizeLevels : 0.0f;
    postData.pixelSize = pp.stylized.pixelateEnabled ? pp.stylized.pixelSize : 0.0f;
    postData.bloomThreshold = pp.bloom.threshold;
    postData.bloomSoftKnee = pp.bloom.softKnee;
    postData.clarityStrength = pp.imageQuality.clarityEnabled ? pp.imageQuality.clarityStrength : 0.0f;
    postData.clarityRadius = pp.imageQuality.clarityRadius;
    postData.shadowLift = pp.imageQuality.shadowHighlightEnabled ? pp.imageQuality.shadowLift : 0.0f;
    postData.highlightCompression = pp.imageQuality.shadowHighlightEnabled ? pp.imageQuality.highlightCompression : 0.0f;
    postData.colorFilterIntensity = pp.imageQuality.colorFilterEnabled ? pp.imageQuality.colorFilterIntensity : 0.0f;
    postData.colorFilter[0] = pp.imageQuality.colorFilter[0];
    postData.colorFilter[1] = pp.imageQuality.colorFilter[1];
    postData.colorFilter[2] = pp.imageQuality.colorFilter[2];
    postData.screenFadeAlpha    = pp.screenFadeAlpha;
    postData.screenFadeColor[0] = pp.screenFadeColor[0];
    postData.screenFadeColor[1] = pp.screenFadeColor[1];
    postData.screenFadeColor[2] = pp.screenFadeColor[2];
    const UnderwaterInfo underwater = EvaluateUnderwaterInfo(ctx);
    if (underwater.enabled) {
        postData.underwaterStrength = underwater.strength;
        postData.underwaterDepth = underwater.depth;
        postData.underwaterColor[0] = underwater.color.x;
        postData.underwaterColor[1] = underwater.color.y;
        postData.underwaterColor[2] = underwater.color.z;
        postData.underwaterFogDensity = underwater.fogDensity;
    }
    resources.Update(h.postprocCB, &postData, sizeof(PostProcCB));

    r.SetSampler(0, renderer::SamplerMode::CLAMP_LINEAR);
    // Procedural Texture3D LUTはテクセル間を三線形補間し、端ではClampする。
    r.SetSampler(2, renderer::SamplerMode::CLAMP_LINEAR);

    renderer::DrawCall compositeDC;
    compositeDC.shader = h.compositeShader;
    compositeDC.pipelineState = h.postprocPSO;
    compositeDC.vertexCount = 3;
    compositeDC.constantBuffers[0] = h.frameCB;
    compositeDC.constantBuffers[5] = h.postprocCB;
    compositeDC.constantBuffers[8] = h.advancedGraphicsCB; // b8: ssrIntensity, volLightIntensity, lutBlend 等
    // MotionBlur が有効な場合、CS が生成した blurred HDR を hdrRT の代わりに t5 に束縛する。
    // WHY: MotionBlurPass が motionBlurResult に完全なブラー済み HDR を書いているため、
    //      Composite はそれを HDR ソースとして読めばよい。Composite.hlsl の変更は不要。
    compositeDC.textures[5] = (rs.motionBlur.enabled && h.motionBlurResult.IsValid())
        ? h.motionBlurResult
        : resources.GetColorTexture(h.hdrRT, 0);
    compositeDC.textures[7] = resources.GetDepthTexture(h.hdrRT);
    compositeDC.textures[10] = pp.bloom.enabled ? h.bloomFull : renderer::ResourceHandle<renderer::TextureTag>{};
    // SSR 反射結果 (t19) — Composite.hlsl が ssrIntensity に基づいてブレンドする
    if (rs.ssr.enabled && h.ssrResult.IsValid())
        compositeDC.textures[19] = h.ssrResult;
    // Volumetric Light 結果 (t20) — Composite.hlsl が volLightIntensity で加算する
    if (rs.volumetricLight.enabled && h.volumetricResult.IsValid())
        compositeDC.textures[20] = h.volumetricResult;
    // Procedural LUT (t22) — Renderer互換の32^3 Texture3DをRenderSystemがCPU生成する。
    if (rs.lutColorGrading.enabled && h.proceduralColorLut.IsValid())
        compositeDC.textures[22] = h.proceduralColorLut;
    r.Submit(compositeDC, resources);

    h.postProcessInput = resources.GetColorTexture(h.ldrRT, 0);
    h.fxaaInput = h.postProcessInput;
}

} // namespace fbzz::scene
