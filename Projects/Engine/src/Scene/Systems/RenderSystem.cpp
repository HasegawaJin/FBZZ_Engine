// FBZZ Engine
// RenderSystem.cpp | fbzz::scene
// Scene render system entry point
// Builds render graph passes and submits renderer draw calls.
#include "Engine/Scene/Systems/RenderSystem.hpp"
#include "Engine/Renderer/RenderSettings.hpp"
#include "RenderPasses/DebugPasses.hpp"
#include "RenderPasses/PostProcessPasses.hpp"
#include "RenderPasses/RenderPassContext.hpp"
#include "RenderPasses/SelectionPasses.hpp"
#include "Engine/Core/Time.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Components/LightComponent.hpp"
#include "Engine/Scene/Components/MeshRenderer.hpp"
#include "Engine/Scene/Components/MaterialComponent.hpp"
#include "Engine/Scene/Components/AnimatorComponent.hpp"
#include "Engine/Scene/Components/SkinnedMeshRenderer.hpp"
#include "Engine/Scene/Components/ParticleEmitter.hpp"
#include "Engine/Scene/Components/SkyRenderer.hpp"
#include "Engine/Scene/Transform.hpp"
#include "Engine/Renderer/IRenderer.hpp"
#include "Engine/Renderer/Camera.hpp"
#include "Engine/Renderer/LightSystem.hpp"
#include "Engine/Renderer/Mesh.hpp"
#include "Engine/Renderer/Material.hpp"
#include "Engine/Renderer/DrawCall.hpp"
#include "Engine/Renderer/PrimitiveMesh.hpp"
#include "Engine/Renderer/RenderGraph.hpp"
#include "Engine/Renderer/RenderState.hpp"
#include "Engine/Renderer/SamplerMode.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include "Engine/Asset/Skeleton.hpp"
#include <Math/Matrix4.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

namespace fbzz::scene {

namespace {

constexpr uint32_t SHADOW_MAP_SIZE = 8192;

// Matches ParticleVSIn in Particle.hlsl.
struct ParticleVertex {
    float center[3];  // POSITION   12 bytes
    float uv[2];      // TEXCOORD0   8 bytes
    float color[4];   // COLOR       16 bytes
    float size;       // TEXCOORD1   4 bytes
};                    // 40 bytes

inline math::Vector4 LerpVec4(const math::Vector4& a, const math::Vector4& b, float t)
{
    return { a.x + (b.x - a.x) * t,
             a.y + (b.y - a.y) * t,
             a.z + (b.z - a.z) * t,
             a.w + (b.w - a.w) * t };
}

renderer::Material* SyncMaterial(MaterialComponent& mc, renderer::ResourceManager& resources)
{
    if (!mc.enabled) return nullptr;

    if (!mc.material)
        mc.material = std::make_shared<renderer::Material>();

    if (mc.material.use_count() > 1) {
        auto cloned = std::make_shared<renderer::Material>(*mc.material);
        cloned->paramsBuffer = renderer::ResourceHandle<renderer::ConstantBufferTag>{};
        mc.material = std::move(cloned);
    }

    auto& material = *mc.material;
    material.shaderPath = mc.shaderPath;
    material.shader = material.shaderPath.empty()
        ? renderer::ResourceHandle<renderer::ShaderTag>{}
        : resources.LoadShader(material.shaderPath);
    material.albedoTexture = mc.albedoTexPath.empty()
        ? renderer::ResourceHandle<renderer::TextureTag>{}
        : resources.LoadTexture(mc.albedoTexPath);
    material.normalTexture = mc.normalTexPath.empty()
        ? renderer::ResourceHandle<renderer::TextureTag>{}
        : resources.LoadTexture(mc.normalTexPath);
    material.Upload(resources);
    return &material;
}

} // namespace

void RenderSystem(Scene& scene,
                  renderer::IRenderer& renderer,
                  renderer::ResourceManager& resources,
                  const renderer::Camera& camera,
                  renderer::ResourceHandle<renderer::RenderTargetTag> outputRT,
                  const renderer::RenderSettings* settings,
                  fbzz::LayerMask cullingMask)
{
    static renderer::RenderSettings sDefaultSettings;
    renderer::RenderSettings effectiveSettings = settings ? *settings : sDefaultSettings;
    if (const auto* runtimePostProcess = scene.TryGetRuntimePostProcessSettings())
        effectiveSettings.postProcess = *runtimePostProcess;
    const renderer::RenderSettings& rs = effectiveSettings;
    static auto shadowMapRT     = resources.CreateRenderTarget(SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, 0);
    static auto shadowShader    = resources.LoadShader("assets/shaders/Pipeline/Shadow/ShadowMap.hlsl");
    static auto skinnedShadowShader = resources.LoadShader("assets/shaders/Pipeline/Shadow/SkinnedShadowMap.hlsl");
    static auto skinnedPbrShader = resources.LoadShader("assets/shaders/Material/Skinned/SkinnedPBR.hlsl");
    // Skinned meshes without AnimatorComponent still bind an identity bone palette.
    // The shader always reads skinning matrices from b7.
    static renderer::ResourceHandle<renderer::ConstantBufferTag> bindPoseSkinningCB;
    if (!bindPoseSkinningCB.IsValid()) {
        struct BindPoseData { math::Matrix4 bones[asset::MAX_SKINNING_BONES]; };
        BindPoseData bp{};
        for (auto& m : bp.bones) m = math::Matrix4::Identity();
        bindPoseSkinningCB = resources.CreateConstantBuffer(sizeof(BindPoseData));
        resources.Update(bindPoseSkinningCB, &bp, sizeof(BindPoseData));
    }
    static auto compositeShader = resources.LoadShader("assets/shaders/PostProcess/Color/Composite.hlsl");
    static auto bloomDownShader = resources.LoadShader("assets/shaders/PostProcess/Bloom/BloomDownsample.cs.hlsl");
    static auto bloomUpShader   = resources.LoadShader("assets/shaders/PostProcess/Bloom/BloomUpsample.cs.hlsl");
    static auto selectionMaskShader        = resources.LoadShader("assets/shaders/Debug/SelectionMask.hlsl");
    static auto selectionMaskSkinnedShader = resources.LoadShader("assets/shaders/Debug/SelectionMaskSkinnedMesh.hlsl");
    static auto selectionOutlineShader     = resources.LoadShader("assets/shaders/PostProcess/Outline/SelectionOutline.hlsl");
    static auto frameCB         = resources.CreateConstantBuffer(sizeof(PerFrameCB));
    static auto objectCB        = resources.CreateConstantBuffer(sizeof(PerObjectCB));
    static auto lightCB         = resources.CreateConstantBuffer(sizeof(renderer::LightConstantsCB));
    static auto shadowCB        = resources.CreateConstantBuffer(sizeof(ShadowConstantsCB));
    static auto postprocCB      = resources.CreateConstantBuffer(sizeof(PostProcCB));
    static auto outlineCB       = resources.CreateConstantBuffer(sizeof(OutlineCB));
    static auto pso             = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE,
        renderer::DepthMode::DEPTH_ON
    });
    static auto wireframePso    = resources.CreatePipelineState({
        renderer::RasterizerMode::WIREFRAME,
        renderer::BlendMode::OPAQUE,
        renderer::DepthMode::DEPTH_ON
    });
    static auto selectionMaskPso = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE,
        renderer::DepthMode::DEPTH_ON
    });
    // Skydome CSO is generated from Material/Sky/Skydome.hlsl.
    static auto skydomeShader = resources.LoadShader("assets/shaders/Material/Sky/Skydome.hlsl");
    static auto skydomePSO    = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::OPAQUE,
        renderer::DepthMode::DEPTH_SKY
    });
    static auto skydomeMesh = renderer::PrimitiveMesh::Sphere(resources, 32);
    static auto atmCB       = resources.CreateConstantBuffer(sizeof(AtmosphereCB));

    static auto particleShader = resources.LoadShader("assets/shaders/Material/Effects/Particle.hlsl");
    static auto particlePSO    = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ADDITIVE,
        renderer::DepthMode::DEPTH_READ
    });
    // Shared full-screen PSO for passes that do not need depth.
    static auto postprocPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE,
        renderer::DepthMode::DEPTH_OFF
    });
    static auto fxaaShader = resources.LoadShader("assets/shaders/PostProcess/AntiAliasing/FXAA.hlsl");

    // Preallocate buffers for the maximum particle draw count.
    static renderer::ResourceHandle<renderer::BufferTag> particleVB;
    static renderer::ResourceHandle<renderer::BufferTag> particleIB;
    constexpr int MAX_PARTICLE_DRAW = 1000;
    constexpr uint32_t MAX_PARTICLE_VERTICES = MAX_PARTICLE_DRAW * 4;
    if (!particleVB.IsValid())
    {
        particleVB = resources.CreateVertexBuffer(
            nullptr,
            MAX_PARTICLE_VERTICES * sizeof(ParticleVertex),
            sizeof(ParticleVertex));
    }
    if (!particleIB.IsValid())
    {
        std::vector<uint32_t> idx;
        idx.reserve(MAX_PARTICLE_DRAW * 6);
        for (int i = 0; i < MAX_PARTICLE_DRAW; ++i) {
            uint32_t b = static_cast<uint32_t>(i * 4);
            idx.insert(idx.end(), { b, b+1, b+2, b+1, b+3, b+2 });
        }
        particleIB = resources.CreateIndexBuffer(idx.data(), static_cast<uint32_t>(idx.size()));
    }

    // Resize render targets with the output size.
    static renderer::ResourceHandle<renderer::RenderTargetTag> hdrRT;
    static renderer::ResourceHandle<renderer::RenderTargetTag> ldrRT;
    static renderer::ResourceHandle<renderer::RenderTargetTag> selectionMaskRT;
    static renderer::ResourceHandle<renderer::RenderTargetTag> outlineRT;
    static renderer::ResourceHandle<renderer::TextureTag>      bloomHalf;
    static renderer::ResourceHandle<renderer::TextureTag>      bloomFull;
    static uint32_t sHdrW = 0, sHdrH = 0;
    {
        const auto* output = resources.Get(outputRT);
        uint32_t curW = output ? output->GetWidth()  : renderer.GetWidth();
        uint32_t curH = output ? output->GetHeight() : renderer.GetHeight();
        if (curW == 0 || curH == 0) return;
        if (!hdrRT.IsValid() || sHdrW != curW || sHdrH != curH)
        {
            if (hdrRT.IsValid()) resources.Release(hdrRT);
            if (ldrRT.IsValid()) resources.Release(ldrRT);
            if (selectionMaskRT.IsValid()) resources.Release(selectionMaskRT);
            if (outlineRT.IsValid()) resources.Release(outlineRT);
            if (bloomHalf.IsValid()) resources.Release(bloomHalf);
            if (bloomFull.IsValid()) resources.Release(bloomFull);
            hdrRT           = resources.CreateRenderTarget(curW, curH, 1);
            ldrRT           = resources.CreateRenderTarget(curW, curH, 1);
            selectionMaskRT = resources.CreateRenderTarget(curW, curH, 1);
            outlineRT       = resources.CreateRenderTarget(curW, curH, 1);
            bloomHalf       = resources.CreateComputeTexture(std::max(1u, curW / 2), std::max(1u, curH / 2));
            bloomFull       = resources.CreateComputeTexture(curW, curH);
            sHdrW           = curW;
            sHdrH           = curH;
        }
    }

    // =========================================================================
    // Build LightConstantsCB from LightComponent data.
    // =========================================================================
    renderer::LightConstantsCB lightData{};
    lightData.lightDir       = { 0.0f, -1.0f, 0.5f };
    lightData.lightColor     = { 1.0f,  1.0f, 1.0f };
    lightData.lightIntensity = 1.0f;

    constexpr float kDegToRad = 3.14159265f / 180.0f;
    for (auto [tf, lc] : scene.View<Transform, LightComponent>()) {
        if (!lc.enabled) continue;
        if (lc.type == LightComponent::Type::Directional) {
            lightData.lightDir       = tf.Forward().Normalized();
            lightData.lightColor     = lc.color;
            lightData.lightIntensity = lc.intensity;
        } else if (lc.type == LightComponent::Type::Point
                   && lightData.pointLightCount < 8) {
            auto& pl    = lightData.pointLights[lightData.pointLightCount++];
            pl.position  = tf.position;
            pl.range     = lc.range;
            pl.color     = lc.color;
            pl.intensity = lc.intensity;
        } else if (lc.type == LightComponent::Type::Spot
                   && lightData.spotLightCount < 4) {
            auto& sl    = lightData.spotLights[lightData.spotLightCount++];
            sl.position  = tf.position;
            sl.direction = tf.Forward().Normalized();
            sl.range     = lc.range;
            sl.innerCos  = std::cos(lc.innerCone * kDegToRad);
            sl.outerCos  = std::cos(lc.outerCone * kDegToRad);
            sl.color     = lc.color;
            sl.intensity = lc.intensity;
        }
    }

    math::Vector3 lightDir = lightData.lightDir.Normalized();
    math::Vector3 sceneCenter = { 0.0f, 1.0f, 4.0f };
    math::Vector3 lightPos    = sceneCenter - lightDir * 30.0f;
    math::Vector3 up = (std::abs(lightDir.y) > 0.99f)
                       ? math::Vector3{ 1.0f, 0.0f, 0.0f }
                       : math::Vector3{ 0.0f, 1.0f, 0.0f };
    math::Matrix4 lightView = math::Matrix4::LookAt(lightPos, sceneCenter, up);
    math::Matrix4 lightProj = math::Matrix4::Orthographic(-20.0f, 20.0f, -20.0f, 20.0f, 1.0f, 60.0f);
    math::Matrix4 lightVP   = lightProj * lightView;

    const bool selectionOutlineEnabled =
        rs.showSelectionOutline && !rs.selectedObjects.empty() &&
        selectionMaskRT.IsValid() && selectionMaskPso.IsValid() &&
        selectionOutlineShader.IsValid();

    RenderPassHandles passHandles{};
    passHandles.shadowMapRT = shadowMapRT;
    passHandles.hdrRT = hdrRT;
    passHandles.ldrRT = ldrRT;
    passHandles.selectionMaskRT = selectionMaskRT;
    passHandles.outlineRT = outlineRT;
    passHandles.bloomHalf = bloomHalf;
    passHandles.bloomFull = bloomFull;
    passHandles.bloomDownShader = bloomDownShader;
    passHandles.bloomUpShader = bloomUpShader;
    passHandles.compositeShader = compositeShader;
    passHandles.selectionMaskShader = selectionMaskShader;
    passHandles.selectionMaskSkinnedShader = selectionMaskSkinnedShader;
    passHandles.selectionOutlineShader = selectionOutlineShader;
    passHandles.fxaaShader = fxaaShader;
    passHandles.selectionMaskPSO = selectionMaskPso;
    passHandles.postprocPSO = postprocPSO;
    passHandles.frameCB = frameCB;
    passHandles.objectCB = objectCB;
    passHandles.bindPoseSkinningCB = bindPoseSkinningCB;
    passHandles.postprocCB = postprocCB;
    passHandles.outlineCB = outlineCB;

    RenderPassContext passCtx{
        scene,
        renderer,
        resources,
        camera,
        rs,
        outputRT,
        cullingMask,
        passHandles,
        sHdrW,
        sHdrH,
        selectionOutlineEnabled
    };

    renderer::RenderGraph graph;
    graph.DeclareResource("Output", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, true, false });
    graph.DeclareResource("ShadowMap", { renderer::RenderGraph::ResourceKind::RenderTarget, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, 0, false, false });
    graph.DeclareResource("HDR", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    graph.DeclareResource("LDR", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    graph.DeclareResource("SelectionMask", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    graph.DeclareResource("Outline", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    graph.DeclareResource("Bloom", { renderer::RenderGraph::ResourceKind::Texture, sHdrW, sHdrH, 0, false, true });
    graph.SetOutputs({ "Output" });

    graph.AddPass("Shadow", {}, { "ShadowMap" }, [&]() {
    renderer.SetRenderTarget(shadowMapRT, resources);
    renderer.ClearDepth();

    if (rs.shadowEnabled)
    {
        PerFrameCB lightFrameData{};
        lightFrameData.viewProjection = lightVP;
        resources.Update(frameCB, &lightFrameData, sizeof(PerFrameCB));

        // Pass 1a: static mesh shadows.
        for (auto& go : scene.GameObjects()) {
            if (!fbzz::Layer::Contains(cullingMask, go.layer)) continue;
            auto* mr  = go.GetComponent<MeshRenderer>();
            auto* mat = go.GetComponent<MaterialComponent>();
            if (!mr || !mr->enabled || !mr->mesh || !mat || !mat->enabled) continue;
            if (!mr->mesh->vertexBuffer.IsValid() || !mr->mesh->indexBuffer.IsValid()) continue;
            if (mr->mesh->isSkinned) continue;

            PerObjectCB objData{};
            objData.world = go.transform.GetWorldMatrix();
            resources.Update(objectCB, &objData, sizeof(PerObjectCB));

            renderer::DrawCall dc;
            dc.vertexBuffer       = mr->mesh->vertexBuffer;
            dc.indexBuffer        = mr->mesh->indexBuffer;
            dc.indexCount         = mr->mesh->indexCount;
            dc.shader             = shadowShader;
            dc.pipelineState      = pso;
            dc.constantBuffers[0] = frameCB;
            dc.constantBuffers[1] = objectCB;
            renderer.Submit(dc, resources);
        }


        if (skinnedShadowShader.IsValid()) {
            for (auto& go : scene.GameObjects()) {
                if (!fbzz::Layer::Contains(cullingMask, go.layer)) continue;
                auto* smr  = go.GetComponent<SkinnedMeshRenderer>();
                auto* mat  = go.GetComponent<MaterialComponent>();
                auto* anim = go.GetComponent<AnimatorComponent>();
                if (!smr || !smr->enabled || !smr->model) continue;
                if (!mat || !mat->enabled) continue;

                PerObjectCB objData{};
                objData.world = go.transform.GetWorldMatrix();
                resources.Update(objectCB, &objData, sizeof(PerObjectCB));

                const auto skinCB = (anim && anim->skinningBuffer.IsValid())
                    ? anim->skinningBuffer : bindPoseSkinningCB;

                for (const auto& meshPtr : smr->model->meshes) {
                    if (!meshPtr) continue;
                    if (!meshPtr->vertexBuffer.IsValid() || !meshPtr->indexBuffer.IsValid()) continue;

                    renderer::DrawCall dc;
                    dc.vertexBuffer       = meshPtr->vertexBuffer;
                    dc.indexBuffer        = meshPtr->indexBuffer;
                    dc.indexCount         = meshPtr->indexCount;
                    dc.shader             = skinnedShadowShader;
                    dc.pipelineState      = pso;
                    dc.constantBuffers[0] = frameCB;
                    dc.constantBuffers[1] = objectCB;
                    dc.constantBuffers[7] = skinCB;
                    renderer.Submit(dc, resources);
                }
            }
        }
    }

    });

    graph.AddPass("ForwardOpaque", { "ShadowMap" }, { "HDR" }, [&]() {
    renderer.SetRenderTarget(hdrRT, resources);
    renderer.Clear({ 0.005f, 0.005f, 0.02f, 1.0f });

    PerFrameCB frameData{};
    frameData.view              = camera.GetViewMatrix();
    frameData.projection        = camera.GetProjectionMatrix();
    frameData.viewProjection    = camera.GetViewProjection();
    frameData.invViewProjection = math::Matrix4::Inverse(frameData.viewProjection);
    frameData.cameraPos         = camera.m_position;
    frameData.nearZ             = camera.m_near;
    frameData.farZ              = camera.m_far;
    resources.Update(frameCB, &frameData, sizeof(PerFrameCB));

    resources.Update(lightCB, &lightData, sizeof(renderer::LightConstantsCB));

    ShadowConstantsCB shadowData{};
    shadowData.lightViewProjection    = lightVP;
    shadowData.shadowMapTexelSize[0]  = 1.0f / static_cast<float>(SHADOW_MAP_SIZE);
    shadowData.shadowMapTexelSize[1]  = 1.0f / static_cast<float>(SHADOW_MAP_SIZE);
    shadowData.shadowBias             = 0.005f;
    resources.Update(shadowCB, &shadowData, sizeof(ShadowConstantsCB));

    renderer.SetSampler(0, renderer::SamplerMode::WRAP_ANISOTROPIC);
    renderer.SetSampler(1, renderer::SamplerMode::BORDER_ZERO);

    passHandles.shadowDepthTex = resources.GetDepthTexture(shadowMapRT);

    // Pass 2a: static meshes.
    for (auto& go : scene.GameObjects()) {
        if (!fbzz::Layer::Contains(cullingMask, go.layer)) continue;
        auto* mr  = go.GetComponent<MeshRenderer>();
        auto* mat = go.GetComponent<MaterialComponent>();
        if (!mr || !mr->enabled || !mr->mesh || !mat) continue;
        if (!mr->mesh->vertexBuffer.IsValid() || !mr->mesh->indexBuffer.IsValid()) continue;
        if (mr->mesh->isSkinned) continue;
        auto* material = SyncMaterial(*mat, resources);
        if (!material || !material->shader.IsValid()) continue;

        PerObjectCB objData{};
        objData.world             = go.transform.GetWorldMatrix();
        objData.worldInvTranspose = math::Matrix4::Transpose(math::Matrix4::Inverse(objData.world));
        resources.Update(objectCB, &objData, sizeof(PerObjectCB));

        renderer::DrawCall dc;
        dc.vertexBuffer       = mr->mesh->vertexBuffer;
        dc.indexBuffer        = mr->mesh->indexBuffer;
        dc.indexCount         = mr->mesh->indexCount;
        dc.vertexCount        = mr->mesh->vertexCount;
        dc.shader             = material->shader;
        dc.pipelineState      = rs.wireframeMode ? wireframePso : pso;
        dc.constantBuffers[0] = frameCB;
        dc.constantBuffers[1] = objectCB;
        dc.constantBuffers[2] = material->paramsBuffer;
        dc.constantBuffers[3] = lightCB;
        dc.constantBuffers[4] = shadowCB;
        if (material->albedoTexture.IsValid()) dc.textures[0] = material->albedoTexture;
        if (material->normalTexture.IsValid()) dc.textures[1] = material->normalTexture;
        dc.textures[8] = passHandles.shadowDepthTex;
        renderer.Submit(dc, resources);
    }

    if (skinnedPbrShader.IsValid()) {
        for (auto& go : scene.GameObjects()) {
            if (!fbzz::Layer::Contains(cullingMask, go.layer)) continue;
            auto* smr  = go.GetComponent<SkinnedMeshRenderer>();
            auto* mat  = go.GetComponent<MaterialComponent>();
            auto* anim = go.GetComponent<AnimatorComponent>();
            if (!smr || !smr->enabled || !smr->model) continue;
            if (!mat) continue;
            auto* material = SyncMaterial(*mat, resources);
            if (!material) continue;

            PerObjectCB objData{};
            objData.world             = go.transform.GetWorldMatrix();
            objData.worldInvTranspose = math::Matrix4::Transpose(math::Matrix4::Inverse(objData.world));
            resources.Update(objectCB, &objData, sizeof(PerObjectCB));

            const auto skinCB = (anim && anim->skinningBuffer.IsValid())
                ? anim->skinningBuffer : bindPoseSkinningCB;

            for (const auto& meshPtr : smr->model->meshes) {
                if (!meshPtr) continue;
                if (!meshPtr->vertexBuffer.IsValid() || !meshPtr->indexBuffer.IsValid()) continue;

                renderer::DrawCall dc;
                dc.vertexBuffer       = meshPtr->vertexBuffer;
                dc.indexBuffer        = meshPtr->indexBuffer;
                dc.indexCount         = meshPtr->indexCount;
                dc.vertexCount        = meshPtr->vertexCount;
                dc.shader             = skinnedPbrShader;
                dc.pipelineState      = rs.wireframeMode ? wireframePso : pso;
                dc.constantBuffers[0] = frameCB;
                dc.constantBuffers[1] = objectCB;
                dc.constantBuffers[2] = material->paramsBuffer;
                dc.constantBuffers[3] = lightCB;
                dc.constantBuffers[4] = shadowCB;
                dc.constantBuffers[7] = skinCB;
                if (material->albedoTexture.IsValid()) dc.textures[0] = material->albedoTexture;
                if (material->normalTexture.IsValid()) dc.textures[1] = material->normalTexture;
                dc.textures[8] = passHandles.shadowDepthTex;
                renderer.Submit(dc, resources);
            }
        }
    }

    });

    graph.AddPass("Sky", { "HDR" }, { "HDR" }, [&]() {
    if (skydomeShader.IsValid() && skydomeMesh && skydomeMesh->vertexBuffer.IsValid() && skydomeMesh->indexBuffer.IsValid())
    {
        for (auto& go : scene.GameObjects()) {
            if (!fbzz::Layer::Contains(cullingMask, go.layer)) continue;
            auto* sky = go.GetComponent<SkyRenderer>();
            if (!sky || !sky->enabled) continue;

            PostProcCB skyPostData{};
            skyPostData.exposure = 1.0f;
            skyPostData.time     = core::Time::TotalTime();
            resources.Update(postprocCB, &skyPostData, sizeof(PostProcCB));

            AtmosphereCB atmData{};
            atmData.rayleighScattering[0] = sky->rayleighScattering.x;
            atmData.rayleighScattering[1] = sky->rayleighScattering.y;
            atmData.rayleighScattering[2] = sky->rayleighScattering.z;
            atmData.mieScattering         = sky->mieScattering;
            atmData.planetRadius          = 6371.0f;
            atmData.atmosphereRadius      = 6471.0f;
            atmData.sunIntensity          = sky->sunIntensity;
            atmData.mieG                  = sky->mieG;
            resources.Update(atmCB, &atmData, sizeof(AtmosphereCB));

            renderer::DrawCall skyDC;
            skyDC.vertexBuffer       = skydomeMesh->vertexBuffer;
            skyDC.indexBuffer        = skydomeMesh->indexBuffer;
            skyDC.indexCount         = skydomeMesh->indexCount;
            skyDC.shader             = skydomeShader;
            skyDC.pipelineState      = skydomePSO;
            skyDC.constantBuffers[0] = frameCB;
            skyDC.constantBuffers[3] = lightCB;
            skyDC.constantBuffers[5] = postprocCB;
            skyDC.constantBuffers[6] = atmCB;
            renderer.Submit(skyDC, resources);
            break;
        }
    }

    });

    graph.AddPass("Particle", { "HDR" }, { "HDR" }, [&]() {
    if (particleShader.IsValid() && particleVB.IsValid() && particleIB.IsValid())
    {
        const float dt = core::Time::DeltaTime();

        for (auto& go : scene.GameObjects()) {
            if (!fbzz::Layer::Contains(cullingMask, go.layer)) continue;
            auto* emitter = go.GetComponent<ParticleEmitter>();
            if (!emitter || !emitter->enabled) continue;
            auto& tf = go.transform;

            emitter->emitAccum += emitter->emitRate * dt;
            while (emitter->emitAccum >= 1.0f
                   && static_cast<int>(emitter->particles.size()) < emitter->maxParticles)
            {
                emitter->emitAccum -= 1.0f;
                Particle p;
                p.position = tf.localPosition + emitter->emitPosition;
                float rx = ((std::rand() / float(RAND_MAX)) * 2.0f - 1.0f) * emitter->velocitySpread;
                float rz = ((std::rand() / float(RAND_MAX)) * 2.0f - 1.0f) * emitter->velocitySpread;
                p.velocity = { emitter->emitVelocity.x + rx,
                               emitter->emitVelocity.y,
                               emitter->emitVelocity.z + rz };
                p.color = emitter->colorStart;
                p.size  = emitter->sizeStart;
                p.age   = 0.0f;
                emitter->particles.push_back(std::move(p));
            }

            for (auto it = emitter->particles.begin(); it != emitter->particles.end(); ) {
                it->age += dt;
                if (it->age >= emitter->lifetime) {
                    it = emitter->particles.erase(it);
                    continue;
                }
                float t = it->age / emitter->lifetime;
                it->position.x += it->velocity.x * dt;
                it->position.y += it->velocity.y * dt;
                it->position.z += it->velocity.z * dt;
                it->velocity.y -= 5.0f * dt;  // 驥榊鴨
                it->color = LerpVec4(emitter->colorStart, emitter->colorEnd, t);
                it->size  = emitter->sizeStart + (emitter->sizeEnd - emitter->sizeStart) * t;
                ++it;
            }

            int count = std::min(static_cast<int>(emitter->particles.size()), MAX_PARTICLE_DRAW);
            if (count == 0) continue;

            static const float kUV[4][2] = { {0,0},{1,0},{0,1},{1,1} };
            std::vector<ParticleVertex> verts;
            verts.reserve(static_cast<size_t>(count * 4));
            for (int i = 0; i < count; ++i) {
                const auto& p = emitter->particles[i];
                for (int c = 0; c < 4; ++c) {
                    ParticleVertex v;
                    v.center[0] = p.position.x;
                    v.center[1] = p.position.y;
                    v.center[2] = p.position.z;
                    v.uv[0]     = kUV[c][0];
                    v.uv[1]     = kUV[c][1];
                    v.color[0]  = p.color.x;
                    v.color[1]  = p.color.y;
                    v.color[2]  = p.color.z;
                    v.color[3]  = p.color.w;
                    v.size      = p.size;
                    verts.push_back(v);
                }
            }

            resources.Update(particleVB, verts.data(), verts.size() * sizeof(ParticleVertex));

            renderer::DrawCall dc;
            dc.vertexBuffer       = particleVB;
            dc.indexBuffer        = particleIB;
            dc.indexCount         = static_cast<uint32_t>(count * 6);
            dc.shader             = particleShader;
            dc.pipelineState      = particlePSO;
            dc.constantBuffers[0] = frameCB;
            renderer.Submit(dc, resources);
        }
    }

    });

    if (selectionOutlineEnabled) {
        graph.AddPass("SelectionMask", { "HDR" }, { "SelectionMask" }, [&]() {
            ExecuteSelectionMaskPass(passCtx);
        });
    }

    graph.AddPass("DebugColliders", { "HDR" }, { "HDR" }, [&]() {
        ExecuteDebugCollidersPass(passCtx);
    });

    if (rs.postProcess.bloom.enabled) {
        graph.AddPass("Bloom", { "HDR" }, { "Bloom" }, [&]() {
            ExecuteBloomPass(passCtx);
        });
    }

    const bool needsLdrIntermediate = rs.postProcess.fxaaEnabled || selectionOutlineEnabled;
    if (rs.postProcess.bloom.enabled) {
        graph.AddPass("Composite", { "HDR", "Bloom" }, { needsLdrIntermediate ? "LDR" : "Output" }, [&]() {
            ExecuteCompositePass(passCtx);
        });
    } else {
        graph.AddPass("Composite", { "HDR" }, { needsLdrIntermediate ? "LDR" : "Output" }, [&]() {
            ExecuteCompositePass(passCtx);
        });
    }

    if (selectionOutlineEnabled) {
        graph.AddPass("SelectionOutline", { "LDR", "SelectionMask" }, { rs.postProcess.fxaaEnabled ? "Outline" : "Output" }, [&]() {
            ExecuteSelectionOutlinePass(passCtx);
        });
    }

    if (rs.postProcess.fxaaEnabled) {
        if (selectionOutlineEnabled) {
            graph.AddPass("FXAA", { "Outline" }, { "Output" }, [&]() {
                ExecuteFxaaPass(passCtx);
            });
        } else {
            graph.AddPass("FXAA", { "LDR" }, { "Output" }, [&]() {
                ExecuteFxaaPass(passCtx);
            });
        }
    }

    const bool graphExecuted = graph.Execute();
    assert(graphExecuted);
    (void)graphExecuted;
}

} // namespace fbzz::scene
