/// @file    ReflectionProbeCapturePass.cpp
/// @brief   ReflectionProbe の空のみ／周辺メッシュ込み動的キャプチャと IBL 畳み込み。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// @note プローブごとに 6 面を毎フレーム描くとゲーム本体の描画より高価になり得るため、更新間隔と
/// @note 明示リクエストで間引く。昼夜変化だけを追う用途には DynamicSky、室内・配置物の反射には
/// @note DynamicScene を使う。
#include "RenderScenePassHelpers.hpp"
#include <Graphics/Effects/RenderProbeInput.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Renderer/ITexture.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/ReflectionProbeComponent.hpp>
#include <Engine/Scene/Components/SkyRenderer.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Matrix4.hpp>
#include <algorithm>
#include <cmath>
#include <memory>

namespace fbzz::scene {
namespace {

struct CubeFaceBasis { math::Vector3 forward; math::Vector3 up; };
constexpr CubeFaceBasis kCubeFaces[6] = {
    {{ 1.0f,  0.0f,  0.0f}, { 0.0f, 1.0f,  0.0f}},
    {{-1.0f,  0.0f,  0.0f}, { 0.0f, 1.0f,  0.0f}},
    {{ 0.0f,  1.0f,  0.0f}, { 0.0f, 0.0f, -1.0f}},
    {{ 0.0f, -1.0f,  0.0f}, { 0.0f, 0.0f,  1.0f}},
    {{ 0.0f,  0.0f,  1.0f}, { 0.0f, 1.0f,  0.0f}},
    {{ 0.0f,  0.0f, -1.0f}, { 0.0f, 1.0f,  0.0f}},
};

bool IsInsideProbe(const ReflectionProbeComponent& probe, const math::Vector3& probePos,
                   const math::Vector3& cameraPos)
{
    const math::Vector3 d = cameraPos - probePos;
    if (probe.boxInfluence) {
        return std::abs(d.x) <= probe.boxExtents.x && std::abs(d.y) <= probe.boxExtents.y
            && std::abs(d.z) <= probe.boxExtents.z;
    }
    return d.LengthSq() <= probe.influenceRadius * probe.influenceRadius;
}

void ReleaseProbeTextures(ReflectionProbeComponent& probe, renderer::ResourceManager& resources)
{
    if (probe.runtimeIrradiance.IsValid()) resources.Release(probe.runtimeIrradiance);
    if (probe.runtimePrefilter.IsValid())  resources.Release(probe.runtimePrefilter);
    probe.runtimeIrradiance = {};
    probe.runtimePrefilter = {};
    probe.runtimePrefilterMipCount = 0;
}

bool CaptureAndBake(RenderPassContext& ctx, GameObject& owner, ReflectionProbeComponent& probe)
{
    auto& h = ctx.handles;
    auto& resources = ctx.resources;
    if (!h.skyShader.IsValid() || !h.skyVB.IsValid() || !h.skyIB.IsValid() || !h.skyCaptureFrameCB.IsValid()) return false;
    const uint32_t resolution = std::clamp(probe.captureResolution, 32u, 1024u);
    if (!probe.runtimeCubeRT.IsValid() || probe.runtimeResolution != resolution) {
        if (probe.runtimeCubeRT.IsValid()) resources.Release(probe.runtimeCubeRT);
        ReleaseProbeTextures(probe, resources);
        probe.runtimeCubeRT = resources.CreateCubemapRenderTarget(resolution, 1);
        probe.runtimeResolution = resolution;
    }
    if (!probe.runtimeCubeRT.IsValid()) return false;

    SkyRenderer* sky = nullptr;
    for (auto& go : ctx.scene.GameObjects()) {
        if (!go.activeInHierarchy()) continue;
        if (auto* candidate = go.GetComponent<SkyRenderer>(); candidate && candidate->enabled) { sky = candidate; break; }
    }
    if (!sky) return false;

    AtmosphereCB atmosphere{};
    atmosphere.rayleighScattering[0] = sky->rayleighScattering.x;
    atmosphere.rayleighScattering[1] = sky->rayleighScattering.y;
    atmosphere.rayleighScattering[2] = sky->rayleighScattering.z;
    atmosphere.mieScattering = sky->mieScattering;
    atmosphere.planetRadius = sky->planetRadius;
    atmosphere.atmosphereRadius = sky->atmosphereRadius;
    atmosphere.sunIntensity = sky->skyScatterIntensity;
    atmosphere.mieG = sky->mieG;
    renderer::RenderReflectionProbeInput input;
    input.position = owner.transform.worldPosition;
    input.sourceIndex = owner.GetID().index;
    input.sourceGeneration = owner.GetID().generation;
    input.captureScene = probe.captureMode == ReflectionProbeCaptureMode::DynamicScene;
    input.target = probe.runtimeCubeRT;
    input.resolution = resolution;
    input.atmosphere = atmosphere;
    renderer::RenderReflectionProbeResult output;
    if (!renderer::CaptureReflectionProbe(ctx, input, output)) return false;
    ReleaseProbeTextures(probe, resources);
    probe.runtimeIrradiance = output.irradiance;
    probe.runtimePrefilter = output.prefilter;
    probe.runtimePrefilterMipCount = output.prefilterMipCount;
    probe.lastCaptureTime = Time::time;
    probe.refreshRequested = false;
    return true;
}

} /// @note namespace

ReflectionProbeComponent* ExecuteReflectionProbeCapturePass(RenderPassContext& ctx)
{
    ReflectionProbeComponent* selected = nullptr;
    float selectedDistanceSq = 0.0f;
    for (auto& go : ctx.scene.GameObjects()) {
        auto* probe = go.GetComponent<ReflectionProbeComponent>();
        if (!probe || !probe->enabled || !go.activeInHierarchy()
            || probe->captureMode == ReflectionProbeCaptureMode::Static) continue;
        const bool due = probe->refreshRequested || probe->lastCaptureTime < -1.0e20f
            || probe->updateInterval <= 0.0f || (Time::time - probe->lastCaptureTime) >= probe->updateInterval;
        if (due) (void)CaptureAndBake(ctx, go, *probe);
        if (!probe->runtimeIrradiance.IsValid() || !probe->runtimePrefilter.IsValid()
            || !IsInsideProbe(*probe, go.transform.worldPosition, ctx.camera.m_position)) continue;
        const math::Vector3 d = ctx.camera.m_position - go.transform.worldPosition;
        const float distanceSq = d.LengthSq();
        if (!selected || distanceSq < selectedDistanceSq) { selected = probe; selectedDistanceSq = distanceSq; }
    }
    return selected;
}

} /// @note namespace fbzz::scene
