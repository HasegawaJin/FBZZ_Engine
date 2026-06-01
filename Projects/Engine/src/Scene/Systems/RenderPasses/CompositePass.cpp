// FBZZ Engine
// CompositePass.cpp | fbzz::scene
// Composite render pass implementation
#include "PostProcessPasses.hpp"
#include "RenderPassContext.hpp"
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
    const float time = core::Time::TotalTime();

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

        const float deepDepth = util::Mathf::Max(water.deepDepth, 0.001f);
        const float t = util::Mathf::Clamp01(depth / deepDepth);
        best.enabled = true;
        best.depth = depth;
        best.strength = util::Mathf::Clamp01(depth / 2.0f);
        best.fogDensity = util::Mathf::Lerp(0.04f, 0.35f, t);
        best.color = {
            util::Mathf::Lerp(water.shallowColor.x, water.deepColor.x, t),
            util::Mathf::Lerp(water.shallowColor.y, water.deepColor.y, t),
            util::Mathf::Lerp(water.shallowColor.z, water.deepColor.z, t)
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
    const bool needsLdrIntermediate =
        pp.fxaaEnabled || ctx.selectionOutlineEnabled ||
        (hasCustomPostProcess && h.customPostProcessRT[0].IsValid());
    r.SetRenderTarget(needsLdrIntermediate ? h.ldrRT : ctx.outputRT, resources);

    PostProcCB postData{};
    postData.texelSize[0] = 1.0f / static_cast<float>(ctx.width);
    postData.texelSize[1] = 1.0f / static_cast<float>(ctx.height);
    postData.screenSize[0] = static_cast<float>(ctx.width);
    postData.screenSize[1] = static_cast<float>(ctx.height);
    postData.exposure = pp.exposure;
    postData.time = core::Time::TotalTime();
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

    renderer::DrawCall compositeDC;
    compositeDC.shader = h.compositeShader;
    compositeDC.pipelineState = h.postprocPSO;
    compositeDC.vertexCount = 3;
    compositeDC.constantBuffers[0] = h.frameCB;
    compositeDC.constantBuffers[5] = h.postprocCB;
    compositeDC.textures[5] = resources.GetColorTexture(h.hdrRT, 0);
    compositeDC.textures[7] = resources.GetDepthTexture(h.hdrRT);
    compositeDC.textures[10] = pp.bloom.enabled ? h.bloomFull : renderer::ResourceHandle<renderer::TextureTag>{};
    r.Submit(compositeDC, resources);

    h.postProcessInput = resources.GetColorTexture(h.ldrRT, 0);
    h.fxaaInput = h.postProcessInput;
}

} // namespace fbzz::scene
