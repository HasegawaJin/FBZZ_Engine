// FBZZ Engine
// RenderSystem.cpp | fbzz::scene
// Scene から DrawCall を生成するオーケストレーター
// 各描画パスの実装は RenderPasses/ 以下の Execute*Pass 関数に委譲する。
#include "Engine/Scene/Systems/RenderSystem.hpp"
#include "Engine/Renderer/RenderSettings.hpp"
#include "Engine/Renderer/RenderDebugOverlay.hpp"
#include "RenderPasses/DecalPass.hpp"
#include "RenderPasses/DebugPasses.hpp"
#include "RenderPasses/GeometryPasses.hpp"
#include "RenderPasses/PostProcessPasses.hpp"
#include "RenderPasses/RenderPassContext.hpp"
#include "RenderPasses/SelectionPasses.hpp"
#include "Engine/Core/Time.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Transform.hpp"
#include "Engine/Scene/Components/LightComponent.hpp"
#include "Engine/Asset/AssetManager.hpp"
#include "Engine/Renderer/IRenderer.hpp"
#include "Engine/Renderer/Camera.hpp"
#include "Engine/Renderer/LightSystem.hpp"
#include "Engine/Renderer/Mesh.hpp"
#include "Engine/Renderer/PrimitiveMesh.hpp"
#include "Engine/Renderer/RenderGraph.hpp"
#include "Engine/Renderer/DrawCall.hpp"
#include "Engine/Renderer/RenderState.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include "Engine/Asset/Skeleton.hpp"
#include <Math/Matrix4.hpp>
#include <Math/Vector3.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::scene {

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

    // =========================================================================
    // 静的リソースの遅延初期化
    // =========================================================================
    static auto shadowMapRT          = resources.CreateRenderTarget(kShadowMapSize, kShadowMapSize, 0);
    static auto shadowShader         = resources.LoadShader("assets/shaders/Pipeline/Shadow/ShadowMap.hlsl");
    static auto skinnedShadowShader  = resources.LoadShader("assets/shaders/Pipeline/Shadow/SkinnedShadowMap.hlsl");
    static auto skinnedPbrShader     = resources.LoadShader("assets/shaders/Material/Skinned/SkinnedPBR.hlsl");

    // スキンドメッシュに AnimatorComponent がない場合のアイデンティティボーンパレット
    static renderer::ResourceHandle<renderer::ConstantBufferTag> bindPoseSkinningCB;
    if (!bindPoseSkinningCB.IsValid()) {
        struct BindPoseData { math::Matrix4 bones[asset::MAX_SKINNING_BONES]; };
        BindPoseData bp{};
        for (auto& m : bp.bones) m = math::Matrix4::Identity();
        bindPoseSkinningCB = resources.CreateConstantBuffer(sizeof(BindPoseData));
        resources.Update(bindPoseSkinningCB, &bp, sizeof(BindPoseData));
    }

    static auto compositeShader         = resources.LoadShader("assets/shaders/PostProcess/Color/Composite.hlsl");
    static auto ssaoShader              = resources.LoadShader("assets/shaders/PostProcess/AmbientOcclusion/SSAO.cs.hlsl");
    static auto ssaoBlurShader          = resources.LoadShader("assets/shaders/PostProcess/AmbientOcclusion/SSAOBlur.cs.hlsl");
    static auto bloomDownShader         = resources.LoadShader("assets/shaders/PostProcess/Bloom/BloomDownsample.cs.hlsl");
    static auto bloomUpShader           = resources.LoadShader("assets/shaders/PostProcess/Bloom/BloomUpsample.cs.hlsl");
    static auto selectionMaskShader     = resources.LoadShader("assets/shaders/Debug/SelectionMask.hlsl");
    static auto selectionMaskSkinnedShader = resources.LoadShader("assets/shaders/Debug/SelectionMaskSkinnedMesh.hlsl");
    static auto selectionOutlineShader  = resources.LoadShader("assets/shaders/PostProcess/Outline/SelectionOutline.hlsl");
    static auto fxaaShader              = resources.LoadShader("assets/shaders/PostProcess/AntiAliasing/FXAA.hlsl");

    static auto skydomeShader = resources.LoadShader("assets/shaders/Material/Sky/Skydome.hlsl");
    static auto skydomeMesh   = renderer::PrimitiveMesh::Sphere(resources, 32);

    static auto gbufferShader          = resources.LoadShader("assets/shaders/Pipeline/Deferred/GBuffer.hlsl");
    static auto deferredLightingShader = resources.LoadShader("assets/shaders/Pipeline/Deferred/DeferredLighting.hlsl");
    static auto depthCopyShader        = resources.LoadShader("assets/shaders/Pipeline/Deferred/DepthCopy.hlsl");

    static auto decalShader     = resources.LoadShader("assets/shaders/Material/Decal/Decal.hlsl");
    static auto decalMaskShader = resources.LoadShader("assets/shaders/Material/Decal/DecalMask.hlsl");

    static auto particleShader = resources.LoadShader("assets/shaders/Material/Effects/Particle.hlsl");

    static auto frameCB    = resources.CreateConstantBuffer(sizeof(PerFrameCB));
    static auto objectCB   = resources.CreateConstantBuffer(sizeof(PerObjectCB));
    static auto lightCB    = resources.CreateConstantBuffer(sizeof(renderer::LightConstantsCB));
    static auto shadowCB   = resources.CreateConstantBuffer(sizeof(ShadowConstantsCB));
    static auto postprocCB = resources.CreateConstantBuffer(sizeof(PostProcCB));
    static auto outlineCB  = resources.CreateConstantBuffer(sizeof(OutlineCB));
    static auto atmCB      = resources.CreateConstantBuffer(sizeof(AtmosphereCB));
    static auto decalCB    = resources.CreateConstantBuffer(sizeof(DecalCB));

    // FIXME: 以下の static ハンドルはデバイスリセット (フルスクリーン切替・GPU ドライバ更新) 時に
    //        無効化されない。DX11 DeviceRemoved 対応を実装する際はここを全面的に見直す。
    //        ResourceManager に Reset() API を追加し、Application ループから呼び出す設計が必要。
    static auto defaultPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE,
        renderer::DepthMode::DEPTH_ON
    });
    static auto wireframePSO = resources.CreatePipelineState({
        renderer::RasterizerMode::WIREFRAME,
        renderer::BlendMode::OPAQUE,
        renderer::DepthMode::DEPTH_ON
    });
    static auto selectionMaskPso = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE,
        renderer::DepthMode::DEPTH_ON
    });
    static auto skydomePSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::OPAQUE,
        renderer::DepthMode::DEPTH_SKY
    });
    static auto particlePSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ADDITIVE,
        renderer::DepthMode::DEPTH_READ
    });
    static auto postprocPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE,
        renderer::DepthMode::DEPTH_OFF
    });
    static auto decalPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_OFF
    });
    static auto decalMaskPso = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE,
        renderer::DepthMode::DEPTH_OFF
    });

    // パーティクルバッファ (最大描画数分を事前確保)
    static renderer::ResourceHandle<renderer::BufferTag> particleVB;
    static renderer::ResourceHandle<renderer::BufferTag> particleIB;
    constexpr uint32_t kMaxParticleVertices = static_cast<uint32_t>(kMaxParticleDraw) * 4u;
    if (!particleVB.IsValid())
    {
        particleVB = resources.CreateVertexBuffer(
            nullptr,
            kMaxParticleVertices * static_cast<uint32_t>(sizeof(ParticleVertex)),
            static_cast<uint32_t>(sizeof(ParticleVertex)));
    }
    if (!particleIB.IsValid())
    {
        std::vector<uint32_t> idx;
        idx.reserve(kMaxParticleDraw * 6);
        for (int i = 0; i < kMaxParticleDraw; ++i) {
            uint32_t b = static_cast<uint32_t>(i * 4);
            idx.insert(idx.end(), { b, b+1, b+2, b+1, b+3, b+2 });
        }
        particleIB = resources.CreateIndexBuffer(idx.data(), static_cast<uint32_t>(idx.size()));
    }

    // レンダーターゲットをウィンドウサイズに合わせてリサイズ
    static renderer::ResourceHandle<renderer::RenderTargetTag> hdrRT;
    static renderer::ResourceHandle<renderer::RenderTargetTag> ldrRT;
    static renderer::ResourceHandle<renderer::RenderTargetTag> selectionMaskRT;
    static renderer::ResourceHandle<renderer::RenderTargetTag> outlineRT;
    static renderer::ResourceHandle<renderer::RenderTargetTag> customPostProcessRT[2];
    static renderer::ResourceHandle<renderer::RenderTargetTag> gbufferRT;
    static renderer::ResourceHandle<renderer::RenderTargetTag> decalDepthRT;
    static renderer::ResourceHandle<renderer::RenderTargetTag> decalMaskRT;
    static renderer::ResourceHandle<renderer::TextureTag>      bloomHalf;
    static renderer::ResourceHandle<renderer::TextureTag>      bloomFull;
    static renderer::ResourceHandle<renderer::TextureTag>      ssaoRaw;
    static renderer::ResourceHandle<renderer::TextureTag>      ssaoBlur;
    static uint32_t sHdrW = 0, sHdrH = 0;
    {
        const auto* output = resources.Get(outputRT);
        uint32_t curW = output ? output->GetWidth()  : renderer.GetWidth();
        uint32_t curH = output ? output->GetHeight() : renderer.GetHeight();
        if (curW == 0 || curH == 0) return;
        if (!hdrRT.IsValid() || sHdrW != curW || sHdrH != curH)
        {
            if (hdrRT.IsValid())            resources.Release(hdrRT);
            if (ldrRT.IsValid())            resources.Release(ldrRT);
            if (selectionMaskRT.IsValid())  resources.Release(selectionMaskRT);
            if (outlineRT.IsValid())        resources.Release(outlineRT);
            if (customPostProcessRT[0].IsValid()) resources.Release(customPostProcessRT[0]);
            if (customPostProcessRT[1].IsValid()) resources.Release(customPostProcessRT[1]);
            if (gbufferRT.IsValid())        resources.Release(gbufferRT);
            if (decalDepthRT.IsValid())     resources.Release(decalDepthRT);
            if (decalMaskRT.IsValid())      resources.Release(decalMaskRT);
            if (bloomHalf.IsValid())        resources.Release(bloomHalf);
            if (bloomFull.IsValid())        resources.Release(bloomFull);
            if (ssaoRaw.IsValid())          resources.Release(ssaoRaw);
            if (ssaoBlur.IsValid())         resources.Release(ssaoBlur);
            hdrRT           = resources.CreateRenderTarget(curW, curH, 1);
            ldrRT           = resources.CreateRenderTarget(curW, curH, 1);
            selectionMaskRT = resources.CreateRenderTarget(curW, curH, 1);
            outlineRT       = resources.CreateRenderTarget(curW, curH, 1);
            customPostProcessRT[0] = resources.CreateRenderTarget(curW, curH, 1);
            customPostProcessRT[1] = resources.CreateRenderTarget(curW, curH, 1);
            gbufferRT       = resources.CreateRenderTarget(curW, curH, 2);
            decalDepthRT    = resources.CreateRenderTarget(curW, curH, 0);
            decalMaskRT     = resources.CreateRenderTarget(curW, curH, 1);
            bloomHalf       = resources.CreateComputeTexture(std::max(1u, curW / 2), std::max(1u, curH / 2));
            bloomFull       = resources.CreateComputeTexture(curW, curH);
            ssaoRaw         = resources.CreateComputeTexture(curW, curH);
            ssaoBlur        = resources.CreateComputeTexture(curW, curH);
            sHdrW = curW;
            sHdrH = curH;
        }
    }

    // =========================================================================
    // ライト定数バッファを構築
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

    math::Vector3 lightDir    = lightData.lightDir.Normalized();
    math::Vector3 sceneCenter = { 0.0f, 1.0f, 4.0f };
    math::Vector3 lightPos    = sceneCenter - lightDir * 30.0f;
    math::Vector3 up = (std::abs(lightDir.y) > 0.99f)
                       ? math::Vector3{ 1.0f, 0.0f, 0.0f }
                       : math::Vector3{ 0.0f, 1.0f, 0.0f };
    math::Matrix4 lightView = math::Matrix4::LookAt(lightPos, sceneCenter, up);
    math::Matrix4 lightProj = math::Matrix4::Orthographic(-20.0f, 20.0f, -20.0f, 20.0f, 1.0f, 60.0f);
    math::Matrix4 lightVP   = lightProj * lightView;

    const bool isDeferred = (rs.pipeline == renderer::RenderingPipeline::Deferred);
    const bool ssaoEnabled =
        isDeferred &&
        rs.postProcess.ambientOcclusion.enabled &&
        ssaoShader.IsValid() &&
        ssaoBlurShader.IsValid() &&
        ssaoRaw.IsValid() &&
        ssaoBlur.IsValid();

    const bool selectionOutlineEnabled =
        rs.showSelectionOutline && !rs.selectedObjects.empty() &&
        selectionMaskRT.IsValid() && selectionMaskPso.IsValid() &&
        selectionOutlineShader.IsValid();

    // =========================================================================
    // RenderPassHandles を組み立て
    // =========================================================================
    RenderPassHandles passHandles{};
    passHandles.shadowMapRT       = shadowMapRT;
    passHandles.hdrRT             = hdrRT;
    passHandles.ldrRT             = ldrRT;
    passHandles.selectionMaskRT   = selectionMaskRT;
    passHandles.outlineRT         = outlineRT;
    passHandles.customPostProcessRT[0] = customPostProcessRT[0];
    passHandles.customPostProcessRT[1] = customPostProcessRT[1];
    passHandles.gbufferRT         = gbufferRT;
    passHandles.bloomHalf         = bloomHalf;
    passHandles.bloomFull         = bloomFull;
    passHandles.ssaoRaw           = ssaoRaw;
    passHandles.ssaoBlur          = ssaoBlur;
    passHandles.ssaoShader        = ssaoShader;
    passHandles.ssaoBlurShader    = ssaoBlurShader;
    passHandles.bloomDownShader   = bloomDownShader;
    passHandles.bloomUpShader     = bloomUpShader;
    passHandles.compositeShader   = compositeShader;
    passHandles.selectionMaskShader       = selectionMaskShader;
    passHandles.selectionMaskSkinnedShader = selectionMaskSkinnedShader;
    passHandles.selectionOutlineShader    = selectionOutlineShader;
    passHandles.fxaaShader        = fxaaShader;
    passHandles.customPostProcessShaders.resize(rs.postProcess.customEffects.size());
    std::vector<uint32_t> customPostProcessIndices;
    customPostProcessIndices.reserve(rs.postProcess.customEffects.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(rs.postProcess.customEffects.size()); ++i) {
        const auto& custom = rs.postProcess.customEffects[i];
        if (!custom.enabled || custom.shaderPath.empty()) continue;
        passHandles.customPostProcessShaders[i] = resources.LoadShader(custom.shaderPath);
        if (passHandles.customPostProcessShaders[i].IsValid())
            customPostProcessIndices.push_back(i);
    }
    passHandles.selectionMaskPSO  = selectionMaskPso;
    passHandles.postprocPSO       = postprocPSO;
    passHandles.frameCB           = frameCB;
    passHandles.objectCB          = objectCB;
    passHandles.lightCB           = lightCB;
    passHandles.bindPoseSkinningCB = bindPoseSkinningCB;
    passHandles.postprocCB        = postprocCB;
    passHandles.outlineCB         = outlineCB;
    passHandles.decalDepthRT      = decalDepthRT;
    passHandles.decalMaskRT       = decalMaskRT;
    passHandles.decalShader       = decalShader;
    passHandles.decalMaskShader   = decalMaskShader;
    passHandles.decalPSO          = decalPSO;
    passHandles.decalMaskPSO      = decalMaskPso;
    passHandles.decalCB           = decalCB;
    // ── ジオメトリ用ハンドル ──────────────────────────────────────────────────
    passHandles.shadowShader         = shadowShader;
    passHandles.shadowSkinnedShader  = skinnedShadowShader;
    passHandles.shadowCB             = shadowCB;
    passHandles.defaultPSO           = defaultPSO;
    passHandles.wireframePSO         = wireframePSO;
    passHandles.skyShader            = skydomeShader;
    passHandles.skyPSO               = skydomePSO;
    if (skydomeMesh) {
        passHandles.skyVB         = skydomeMesh->vertexBuffer;
        passHandles.skyIB         = skydomeMesh->indexBuffer;
        passHandles.skyIndexCount = skydomeMesh->indexCount;
    }
    passHandles.atmosphereCB         = atmCB;
    passHandles.particleShader       = particleShader;
    passHandles.particlePSO          = particlePSO;
    passHandles.particleVB           = particleVB;
    passHandles.particleIB           = particleIB;
    passHandles.gbufferShader        = gbufferShader;
    passHandles.deferredLightingShader = deferredLightingShader;
    passHandles.depthCopyShader      = depthCopyShader;
    passHandles.skinnedPbrShader     = skinnedPbrShader;

    RenderPassContext passCtx{
        scene, renderer, resources, camera, rs,
        outputRT, cullingMask, passHandles,
        sHdrW, sHdrH, selectionOutlineEnabled,
        lightData, lightVP, isDeferred, ssaoEnabled
    };

    // =========================================================================
    // RenderGraph にパスを登録
    // =========================================================================
    renderer::RenderGraph graph;
    graph.DeclareResource("Output",     { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, true,  false });
    graph.DeclareResource("ShadowMap",  { renderer::RenderGraph::ResourceKind::RenderTarget, kShadowMapSize, kShadowMapSize, 0, false, false });
    graph.DeclareResource("HDR",        { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    graph.DeclareResource("LDR",        { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    graph.DeclareResource("SelectionMask", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    graph.DeclareResource("Outline",    { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    graph.DeclareResource("CustomPostProcess0", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    graph.DeclareResource("CustomPostProcess1", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    graph.DeclareResource("Bloom",      { renderer::RenderGraph::ResourceKind::Texture, sHdrW, sHdrH, 0, false, true });
    if (isDeferred)
        graph.DeclareResource("GBuffer", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, false });
    if (ssaoEnabled)
        graph.DeclareResource("SSAO", { renderer::RenderGraph::ResourceKind::Texture, sHdrW, sHdrH, 0, false, true });
    graph.SetOutputs({ "Output" });

    // ── Shadow ────────────────────────────────────────────────────────────────
    graph.AddPass("Shadow", {}, { "ShadowMap" }, [&]() {
        ExecuteShadowPass(passCtx);
    });

    // ── Forward or Deferred ───────────────────────────────────────────────────
    if (!isDeferred) {
        graph.AddPass("ForwardOpaque", { "ShadowMap" }, { "HDR" }, [&]() {
            ExecuteForwardPasses(passCtx);
        });
    }

    if (isDeferred) {
        graph.AddPass("DeferredGBuffer", { "ShadowMap" }, { "GBuffer" }, [&]() {
            ExecuteGBufferPass(passCtx);
        });

        graph.AddPass("DeferredDepthCopy", { "GBuffer" }, { "HDR" }, [&]() {
            ExecuteDeferredDepthCopyPass(passCtx);
        });
    }

    // ── Sky ───────────────────────────────────────────────────────────────────
    graph.AddPass("Sky", { "HDR" }, { "HDR" }, [&]() {
        ExecuteSkyPass(passCtx);
    });

    // ── SSAO + Deferred Lighting ──────────────────────────────────────────────
    if (isDeferred) {
        if (ssaoEnabled) {
            graph.AddPass("SSAO", { "GBuffer" }, { "SSAO" }, [&]() {
                ExecuteSSAOPass(passCtx);
            });
            graph.AddPass("DeferredLighting", { "GBuffer", "HDR", "SSAO" }, { "HDR" }, [&]() {
                ExecuteDeferredLightingPass(passCtx);
            });
        } else {
            graph.AddPass("DeferredLighting", { "GBuffer", "HDR" }, { "HDR" }, [&]() {
                ExecuteDeferredLightingPass(passCtx);
            });
        }

        graph.AddPass("DeferredSkinnedForward", { "HDR" }, { "HDR" }, [&]() {
            ExecuteDeferredSkinnedForwardPass(passCtx);
        });

        graph.AddPass("DeferredForwardTransparent", { "HDR" }, { "HDR" }, [&]() {
            ExecuteDeferredForwardTransparentPass(passCtx);
        });
    }

    // ── デカール用深度スナップショット ────────────────────────────────────────
    graph.DeclareResource("DecalDepth", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    graph.AddPass("DecalDepthCopy", { isDeferred ? "GBuffer" : "HDR" }, { "DecalDepth" }, [&]() {
        renderer.SetRenderTarget(decalDepthRT, resources);
        renderer.ClearDepth();
        if (depthCopyShader.IsValid()) {
            renderer::DrawCall dc;
            dc.shader        = depthCopyShader;
            dc.pipelineState = defaultPSO;
            dc.vertexCount   = 3;
            dc.textures[7]   = isDeferred
                ? resources.GetDepthTexture(gbufferRT)
                : resources.GetDepthTexture(hdrRT);
            renderer.Submit(dc, resources);
        }
    });

    // ── Decal + Particle ──────────────────────────────────────────────────────
    graph.AddPass("Decal", { "HDR", "DecalDepth" }, { "HDR" }, [&]() {
        ExecuteDecalPass(passCtx);
    });

    graph.AddPass("Particle", { "HDR" }, { "HDR" }, [&]() {
        ExecuteParticlePass(passCtx);
    });

    // ── Selection / Debug ─────────────────────────────────────────────────────
    if (selectionOutlineEnabled) {
        graph.AddPass("SelectionMask", { "HDR" }, { "SelectionMask" }, [&]() {
            ExecuteSelectionMaskPass(passCtx);
        });
    }

    graph.AddPass("DebugColliders", { "HDR" }, { "HDR" }, [&]() {
        ExecuteDebugCollidersPass(passCtx);
    });

    graph.AddPass("DebugDecalBounds", { "HDR" }, { "HDR" }, [&]() {
        ExecuteDecalDebugPass(passCtx);
    });

    // ── PostProcess チェーン ──────────────────────────────────────────────────
    if (rs.postProcess.bloom.enabled) {
        graph.AddPass("Bloom", { "HDR" }, { "Bloom" }, [&]() {
            ExecuteBloomPass(passCtx);
        });
    }

    const bool customPostProcessEnabled =
        !customPostProcessIndices.empty() &&
        customPostProcessRT[0].IsValid() &&
        customPostProcessRT[1].IsValid();
    const bool needsLdrIntermediate =
        rs.postProcess.fxaaEnabled || selectionOutlineEnabled || customPostProcessEnabled;

    if (rs.postProcess.bloom.enabled) {
        graph.AddPass("Composite", { "HDR", "Bloom" }, { needsLdrIntermediate ? "LDR" : "Output" }, [&]() {
            ExecuteCompositePass(passCtx);
        });
    } else {
        graph.AddPass("Composite", { "HDR" }, { needsLdrIntermediate ? "LDR" : "Output" }, [&]() {
            ExecuteCompositePass(passCtx);
        });
    }

    std::string postCustomResource = "LDR";
    if (customPostProcessEnabled) {
        std::string inputResource = "LDR";
        for (uint32_t passIndex = 0; passIndex < static_cast<uint32_t>(customPostProcessIndices.size()); ++passIndex) {
            const bool lastCustomPass = passIndex + 1 == static_cast<uint32_t>(customPostProcessIndices.size());
            const bool writesOutput   = lastCustomPass && !rs.postProcess.fxaaEnabled && !selectionOutlineEnabled;
            const uint32_t outputIndex = writesOutput ? 2u : (passIndex % 2u);
            const std::string outputResource = writesOutput
                ? std::string("Output")
                : std::string(outputIndex == 0 ? "CustomPostProcess0" : "CustomPostProcess1");
            const std::string passName    = "CustomPostProcess" + std::to_string(passIndex);
            const uint32_t    customIndex = customPostProcessIndices[passIndex];

            graph.AddPass(
                std::string_view(passName),
                { std::string_view(inputResource) },
                { std::string_view(outputResource) },
                [&, customIndex, outputIndex]() {
                    ExecuteCustomPostProcessPass(passCtx, customIndex, outputIndex);
                });

            inputResource = outputResource;
        }
        postCustomResource = inputResource;
    }

    if (selectionOutlineEnabled) {
        graph.AddPass("SelectionOutline",
            { std::string_view(postCustomResource), "SelectionMask" },
            { rs.postProcess.fxaaEnabled ? "Outline" : "Output" },
            [&]() { ExecuteSelectionOutlinePass(passCtx); });
    }

    if (rs.postProcess.fxaaEnabled) {
        if (selectionOutlineEnabled) {
            graph.AddPass("FXAA", { "Outline" }, { "Output" }, [&]() { ExecuteFxaaPass(passCtx); });
        } else if (customPostProcessEnabled) {
            graph.AddPass("FXAA", { std::string_view(postCustomResource) }, { "Output" }, [&]() { ExecuteFxaaPass(passCtx); });
        } else {
            graph.AddPass("FXAA", { "LDR" }, { "Output" }, [&]() { ExecuteFxaaPass(passCtx); });
        }
    }

    // =========================================================================
    // RenderGraph 実行 + デバッグスナップショット更新
    // =========================================================================
    const bool graphExecuted = graph.Execute();
    assert(graphExecuted);
    (void)graphExecuted;

    {
        renderer::RenderDebugOverlay::Snapshot dbgSnap;
        dbgSnap.hdrRT           = hdrRT;
        dbgSnap.ldrRT           = ldrRT;
        dbgSnap.selectionMaskRT = selectionMaskRT;
        dbgSnap.outlineRT       = outlineRT;
        dbgSnap.gbufferRT       = gbufferRT;
        dbgSnap.width           = sHdrW;
        dbgSnap.height          = sHdrH;
        for (const auto& profile : graph.GetLastReport().profiles)
            dbgSnap.passTimings.push_back({ profile.name, profile.cpuMilliseconds });
        renderer::RenderDebugOverlay::UpdateSnapshot(dbgSnap, rs.passViewerEnabled);
    }
}

} // namespace fbzz::scene
