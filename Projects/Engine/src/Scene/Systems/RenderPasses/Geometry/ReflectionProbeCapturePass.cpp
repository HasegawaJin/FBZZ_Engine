// FBZZ Engine
// ReflectionProbeCapturePass.cpp | fbzz::scene
// ReflectionProbe の空のみ／周辺メッシュ込み動的キャプチャと IBL 畳み込み
//
// WHY: プローブごとに 6 面を毎フレーム描くとゲーム本体の描画より高価になり得る。
//      そのため更新間隔と明示リクエストで更新を間引き、昼夜変化だけを追従したい用途には
//      DynamicSky、室内・配置物の反射には DynamicScene を提供する。
#include "GeometryPasses.hpp"
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

void RenderSkyFace(RenderPassContext& ctx, const math::Vector3& position, uint32_t face,
                   renderer::ResourceHandle<renderer::RenderTargetTag> target, SkyRenderer& sky)
{
    auto& h = ctx.handles;
    auto& resources = ctx.resources;
    const math::Matrix4 projection = math::Matrix4::Perspective(math::ToRad(90.0f), 1.0f, 0.1f, 500.0f);
    const math::Matrix4 view = math::Matrix4::LookAt(position, position + kCubeFaces[face].forward, kCubeFaces[face].up);
    PerFrameCB frame{};
    frame.view = view;
    frame.projection = projection;
    frame.viewProjection = projection * view;
    frame.invViewProjection = math::Matrix4::Inverse(frame.viewProjection);
    frame.cameraPos = position;
    frame.nearZ = 0.1f;
    frame.farZ = 500.0f;
    resources.Update(h.skyCaptureFrameCB, &frame, sizeof(frame));
    ctx.renderer.SetRenderTargetFace(target, face, 0, resources);

    renderer::DrawCall skyCall{};
    skyCall.vertexBuffer = h.skyVB;
    skyCall.indexBuffer = h.skyIB;
    skyCall.indexCount = h.skyIndexCount;
    skyCall.shader = h.skyShader;
    skyCall.pipelineState = h.skyPSO;
    skyCall.constantBuffers[0] = h.skyCaptureFrameCB;
    skyCall.constantBuffers[3] = h.lightCB;
    skyCall.constantBuffers[5] = h.postprocCB;
    skyCall.constantBuffers[6] = h.atmosphereCB;
    ctx.renderer.Submit(skyCall, resources);
}

void RenderSceneFace(RenderPassContext& ctx, const GameObject& probeObject,
                     const math::Vector3& position, uint32_t face)
{
    // WHAT: DynamicScene は通常 MeshRenderer の不透明・半透明マテリアルを既存 MaterialComponent
    //       経路で描く。キャプチャ自身は反射へ混入させない。スキンドメッシュ・粒子・UI は時間依存で
    //       コストも大きいため対象外とし、必要なら更新間隔を短くした専用プローブを配置する。
    auto& h = ctx.handles;
    auto& resources = ctx.resources;
    const math::Matrix4 projection = math::Matrix4::Perspective(math::ToRad(90.0f), 1.0f, 0.1f, 500.0f);
    const math::Matrix4 view = math::Matrix4::LookAt(position, position + kCubeFaces[face].forward, kCubeFaces[face].up);
    PerFrameCB frame{};
    frame.view = view;
    frame.projection = projection;
    frame.viewProjection = projection * view;
    frame.invViewProjection = math::Matrix4::Inverse(frame.viewProjection);
    frame.cameraPos = position;
    frame.nearZ = 0.1f;
    frame.farZ = 500.0f;
    resources.Update(h.frameCB, &frame, sizeof(frame));

    for (auto& go : ctx.scene.GameObjects()) {
        if (&go == &probeObject || !ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* meshRenderer = go.GetComponent<MeshRenderer>();
        auto* material = go.GetComponent<MaterialComponent>();
        if (!meshRenderer || !meshRenderer->enabled || !meshRenderer->lodVisible || !meshRenderer->mesh
            || !material || !material->EnsureMaterialAsset()) continue;
        const auto* mesh = meshRenderer->mesh;
        if (!mesh || mesh->isSkinned || !mesh->vertexBuffer.IsValid() || !mesh->indexBuffer.IsValid()) continue;
        auto* gpuMaterial = SyncMaterial(*material, resources);
        if (!gpuMaterial || !gpuMaterial->shader.IsValid()) continue;

        PerObjectCB object{};
        object.world = go.transform.GetWorldMatrix();
        object.worldInvTranspose = math::Matrix4::Transpose(math::Matrix4::Inverse(object.world));
        resources.Update(h.objectCB, &object, sizeof(object));

        renderer::DrawCall draw{};
        draw.vertexBuffer = mesh->vertexBuffer;
        draw.indexBuffer = mesh->indexBuffer;
        draw.indexCount = mesh->indexCount;
        draw.vertexCount = mesh->vertexCount;
        draw.shader = gpuMaterial->shader;
        draw.pipelineState = GetOrCreateMaterialPSO(resources, material->GetBlendMode(), material->IsDoubleSided());
        draw.constantBuffers[0] = h.frameCB;
        draw.constantBuffers[1] = h.objectCB;
        draw.constantBuffers[2] = gpuMaterial->paramsBuffer;
        draw.constantBuffers[3] = h.lightCB;
        draw.constantBuffers[4] = h.shadowCB;
        for (size_t i = 0; i < gpuMaterial->textures.size() && i < 8; ++i)
            if (gpuMaterial->textures[i].IsValid()) draw.textures[i] = gpuMaterial->textures[i];
        draw.textures[8] = resources.GetDepthTexture(h.shadowMapRT);
        ctx.renderer.Submit(draw, resources);
    }
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
        if (auto* candidate = go.GetComponent<SkyRenderer>(); candidate && candidate->enabled) { sky = candidate; break; }
    }
    if (!sky) return false;

    resources.Update(h.lightCB, &ctx.lightData, sizeof(ctx.lightData));
    PostProcCB post{};
    post.exposure = 1.0f;
    post.time = Time::time;
    resources.Update(h.postprocCB, &post, sizeof(post));
    AtmosphereCB atmosphere{};
    atmosphere.rayleighScattering[0] = sky->rayleighScattering.x;
    atmosphere.rayleighScattering[1] = sky->rayleighScattering.y;
    atmosphere.rayleighScattering[2] = sky->rayleighScattering.z;
    atmosphere.mieScattering = sky->mieScattering;
    atmosphere.planetRadius = sky->planetRadius;
    atmosphere.atmosphereRadius = sky->atmosphereRadius;
    atmosphere.sunIntensity = sky->sunIntensity;
    atmosphere.mieG = sky->mieG;
    resources.Update(h.atmosphereCB, &atmosphere, sizeof(atmosphere));

    for (uint32_t face = 0; face < 6; ++face) {
        RenderSkyFace(ctx, owner.transform.worldPosition, face, probe.runtimeCubeRT, *sky);
        if (probe.captureMode == ReflectionProbeCaptureMode::DynamicScene)
            RenderSceneFace(ctx, owner, owner.transform.worldPosition, face);
    }
    ctx.renderer.SetRenderTarget({}, resources);

    constexpr uint32_t kIrradianceSize = 32;
    constexpr uint32_t kPrefilterMips = 5;
    std::unique_ptr<renderer::ITexture> irradiance, prefilter;
    if (!ctx.renderer.BakeSkyLight(probe.runtimeCubeRT, resources, kIrradianceSize,
        resolution, kPrefilterMips, 128, irradiance, prefilter) || !irradiance || !prefilter) return false;
    ReleaseProbeTextures(probe, resources);
    probe.runtimeIrradiance = resources.RegisterTexture(std::move(irradiance));
    probe.runtimePrefilter = resources.RegisterTexture(std::move(prefilter));
    probe.runtimePrefilterMipCount = kPrefilterMips;
    probe.lastCaptureTime = Time::time;
    probe.refreshRequested = false;
    return true;
}

} // namespace

ReflectionProbeComponent* ExecuteReflectionProbeCapturePass(RenderPassContext& ctx)
{
    ReflectionProbeComponent* selected = nullptr;
    float selectedDistanceSq = 0.0f;
    for (auto& go : ctx.scene.GameObjects()) {
        auto* probe = go.GetComponent<ReflectionProbeComponent>();
        if (!probe || !probe->enabled || probe->captureMode == ReflectionProbeCaptureMode::Static) continue;
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

} // namespace fbzz::scene
