// FBZZ Engine
// RenderSystem.cpp | fbzz::scene
// Scene から DrawCall を生成するオーケストレーター
// 各描画パスの実装は RenderPasses/ 以下の Execute*Pass 関数に委譲する。
#include "Engine/Scene/Systems/RenderSystem.hpp"
#include "Engine/Scene/Systems/TerrainRenderSystem.hpp"
#include "Engine/Scene/Systems/WaterRenderSystem.hpp"
#include "Engine/Renderer/RenderSettings.hpp"
#include "Engine/Renderer/RenderDebugOverlay.hpp"
#include "Engine/Renderer/DebugDraw.hpp"
#include "RenderPasses/DebugPasses.hpp"
#include "RenderPasses/GeometryPasses.hpp"
#include "RenderPasses/PostProcessPasses.hpp"
#include <Engine/Scene/Systems/RenderPassContext.hpp>
#include "RenderPasses/SelectionPasses.hpp"
#include "Engine/Core/Time.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/ScriptComponent.hpp"
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
#include <Engine/Profiler/ProfileScope.hpp>
#include <Math/Frustum.hpp>
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
                  fbzz::LayerMask cullingMask,
                  const RenderSystemUIOptions* uiOptions)
{
    FBZZ_PROFILE_SCOPE("RenderSystem");

    static renderer::RenderSettings sDefaultSettings;
    renderer::RenderSettings effectiveSettings = settings ? *settings : sDefaultSettings;
    if (const auto* runtimePostProcess = scene.TryGetRuntimePostProcessSettings())
        effectiveSettings.postProcess = *runtimePostProcess;
    const renderer::RenderSettings& rs = effectiveSettings;

    // =========================================================================
    // 静的リソースの遅延初期化
    // =========================================================================
    static auto shadowMapRT          = resources.CreateRenderTarget(kShadowMapSize, kShadowMapSize, 0);
    static auto shadowShader         = resources.LoadShader("Assets/Shaders/Pipeline/Shadow/ShadowMap.hlsl");
    static auto skinnedShadowShader  = resources.LoadShader("Assets/Shaders/Pipeline/Shadow/SkinnedShadowMap.hlsl");

    // スキンドメッシュに AnimatorComponent がない場合のアイデンティティボーンパレット
    static renderer::ResourceHandle<renderer::ConstantBufferTag> bindPoseSkinningCB;
    if (!bindPoseSkinningCB.IsValid()) {
        struct BindPoseData { math::Matrix4 bones[asset::MAX_SKINNING_BONES]; };
        BindPoseData bp{};
        for (auto& m : bp.bones) m = math::Matrix4::Identity();
        bindPoseSkinningCB = resources.CreateConstantBuffer(sizeof(BindPoseData));
        resources.Update(bindPoseSkinningCB, &bp, sizeof(BindPoseData));
    }

    static auto compositeShader         = resources.LoadShader("Assets/Shaders/PostProcess/Color/Composite.hlsl");
    static auto copyColorShader         = resources.LoadShader("Assets/Shaders/PostProcess/Color/CopyColor.hlsl");
    static auto causticsShader          = resources.LoadShader("Assets/Shaders/PostProcess/Water/Caustics.hlsl");
    static auto ssaoShader              = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/SSAO.cs.hlsl");
    static auto ssaoBlurShader          = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/SSAOBlur.cs.hlsl");
    static auto bloomDownShader         = resources.LoadShader("Assets/Shaders/PostProcess/Bloom/BloomDownsample.cs.hlsl");
    static auto bloomUpShader           = resources.LoadShader("Assets/Shaders/PostProcess/Bloom/BloomUpsample.cs.hlsl");
    static auto selectionMaskShader     = resources.LoadShader("Assets/Shaders/Debug/SelectionMask.hlsl");
    static auto selectionMaskSkinnedShader = resources.LoadShader("Assets/Shaders/Debug/SelectionMaskSkinnedMesh.hlsl");
    static auto selectionOutlineShader  = resources.LoadShader("Assets/Shaders/PostProcess/Outline/SelectionOutline.hlsl");
    static auto fxaaShader              = resources.LoadShader("Assets/Shaders/PostProcess/AntiAliasing/FXAA.hlsl");

    static auto skydomeShader = resources.LoadShader("Assets/Shaders/Material/Sky/Skydome.hlsl");
    static auto skydomeMesh   = renderer::PrimitiveMesh::Sphere(resources, 32);

    static auto gbufferShader          = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/GBuffer.hlsl");
    static auto deferredLightingShader = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DeferredLighting.hlsl");
    static auto depthCopyShader        = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DepthCopy.hlsl");

    static auto decalShader     = resources.LoadShader("Assets/Shaders/Material/Decal/Decal.hlsl");
    static auto decalMaskShader = resources.LoadShader("Assets/Shaders/Material/Decal/DecalMask.hlsl");

    static auto particleShader = resources.LoadShader("Assets/Shaders/Material/Effects/Particle.hlsl");
    static auto trailShader    = resources.LoadShader("Assets/Shaders/Material/Effects/Trail.hlsl");
    static auto meshTrailShader = resources.LoadShader("Assets/Shaders/Material/Effects/MeshTrail.hlsl");
    static auto skinnedMeshTrailShader = resources.LoadShader("Assets/Shaders/Material/Effects/SkinnedMeshTrail.hlsl");

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
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_ON
    });
    static auto wireframePSO = resources.CreatePipelineState({
        renderer::RasterizerMode::WIREFRAME,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_ON
    });
    static auto selectionMaskPso = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_ON
    });
    static auto skydomePSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_SKY
    });
    static auto particlePSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ADDITIVE,
        renderer::DepthMode::DEPTH_READ
    });
    static auto trailPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_READ
    });
    static auto meshTrailPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_READ
    });
    static auto meshTrailDoubleSidedPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_READ
    });
    static auto postprocPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_OFF
    });
    static auto causticsPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::ADDITIVE,
        renderer::DepthMode::DEPTH_OFF
    });
    static auto decalPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_OFF
    });
    static auto decalMaskPso = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE_BLEND,
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
    static renderer::ResourceHandle<renderer::RenderTargetTag> sceneColorRT;
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
            if (sceneColorRT.IsValid())     resources.Release(sceneColorRT);
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
            sceneColorRT    = resources.CreateRenderTarget(curW, curH, 1);
            customPostProcessRT[0] = resources.CreateRenderTarget(curW, curH, 1);
            customPostProcessRT[1] = resources.CreateRenderTarget(curW, curH, 1);
            gbufferRT       = resources.CreateRenderTarget(curW, curH, 2);
            decalDepthRT    = resources.CreateRenderTarget(curW, curH, 0);
            decalMaskRT     = resources.CreateRenderTarget(curW, curH, 1);
            bloomHalf       = resources.CreateComputeTexture((std::max)(1u, curW / 2), (std::max)(1u, curH / 2));
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

    // ambientColor: Lit モードでは AMBIENT_SCALE 相当値、Unlit 系では白に上書き
    lightData.ambientColor = { 0.08f, 0.08f, 0.08f };
    if (rs.IsUnlit()) {
        lightData.ambientColor    = { 1.0f, 1.0f, 1.0f };
        lightData.lightIntensity  = 0.0f;
        lightData.pointLightCount = 0;
        lightData.spotLightCount  = 0;
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
    passHandles.causticsShader    = causticsShader;
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
    passHandles.causticsPSO       = causticsPSO;
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
    passHandles.trailShader          = trailShader;
    passHandles.trailPSO             = trailPSO;
    passHandles.meshTrailShader      = meshTrailShader;
    passHandles.skinnedMeshTrailShader = skinnedMeshTrailShader;
    passHandles.meshTrailPSO         = meshTrailPSO;
    passHandles.meshTrailDoubleSidedPSO = meshTrailDoubleSidedPSO;
    passHandles.gbufferShader        = gbufferShader;
    passHandles.deferredLightingShader = deferredLightingShader;
    passHandles.depthCopyShader      = depthCopyShader;

    // カメラ視錐台とライト視錐台を事前に抽出する。
    // WHY: Gribb-Hartmann 法は VP 行列の各行の和・差から 6 平面を直接導出するため
    //      逆行列を使わず高速に抽出できる。全ジオメトリパスで共有する。
    const math::Frustum cameraFrustum = math::Frustum::FromViewProjection(camera.GetViewProjection());
    const math::Frustum lightFrustum  = math::Frustum::FromViewProjection(lightVP);
    OcclusionCuller occlusionCuller;

    RenderPassContext passCtx{
        scene, renderer, resources, camera, rs,
        outputRT, cullingMask, passHandles,
        sHdrW, sHdrH, selectionOutlineEnabled,
        lightData, lightVP, isDeferred, ssaoEnabled,
        &cameraFrustum, &lightFrustum, &occlusionCuller
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
    graph.DeclareResource("SceneColor", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    graph.DeclareResource("CustomPostProcess0", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    graph.DeclareResource("CustomPostProcess1", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    graph.DeclareResource("Bloom",      { renderer::RenderGraph::ResourceKind::Texture, sHdrW, sHdrH, 0, false, true });
    if (isDeferred)
        graph.DeclareResource("GBuffer", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, false });
    if (ssaoEnabled)
        graph.DeclareResource("SSAO", { renderer::RenderGraph::ResourceKind::Texture, sHdrW, sHdrH, 0, false, true });
    graph.SetOutputs({ "Output" });

    scene.ClearUserRenderPasses();

    auto appendQueuedUserPasses = [&](UserRenderPassInjectionPoint injectionPoint) {
        for (const auto& desc : scene.GetUserRenderPasses()) {
            if (desc.injectionPoint != injectionPoint)
                continue;
            assert(!desc.name.empty() && "UserRenderPassDesc.name is required");
            auto execute = desc.execute;
            graph.AddPass(desc.name, desc.accesses, [&, execute]() {
                if (execute)
                    execute(passCtx);
            }, desc.allowCulling);
        }
    };

    auto queueWaterPasses = [&]() {
        // WHY: Water も Script と同じ UserRenderPassDesc 経路で登録する。
        //      これにより組み込み機能とユーザー VFX が同じ RenderGraph 拡張モデルに乗り、
        //      将来的に Sandbox 側の専用 Script へ移しても RenderSystem の構造を変えずに済む。
        UserRenderPassDesc copyPass{};
        copyPass.name = "WaterSceneColorCopy";
        copyPass.injectionPoint = UserRenderPassInjectionPoint::AfterTransparent;
        copyPass.accesses = {
            { "HDR", renderer::RenderGraph::ResourceUsage::Read },
            { "SceneColor", renderer::RenderGraph::ResourceUsage::Write }
        };
        copyPass.execute = [=](RenderPassContext& ctx) {
            ctx.renderer.SetRenderTarget(sceneColorRT, ctx.resources);
            ctx.renderer.SetSampler(0, renderer::SamplerMode::CLAMP_LINEAR);
            if (copyColorShader.IsValid()) {
                renderer::DrawCall dc;
                dc.shader = copyColorShader;
                dc.pipelineState = postprocPSO;
                dc.vertexCount = 3;
                dc.textures[5] = ctx.resources.GetColorTexture(hdrRT, 0);
                ctx.renderer.Submit(dc, ctx.resources);
            }
        };
        scene.QueueUserRenderPass(std::move(copyPass));

        UserRenderPassDesc waterPass{};
        waterPass.name = "WaterForward";
        waterPass.injectionPoint = UserRenderPassInjectionPoint::AfterTransparent;
        waterPass.accesses = {
            { "HDR", renderer::RenderGraph::ResourceUsage::ReadWrite },
            { "SceneColor", renderer::RenderGraph::ResourceUsage::Read }
        };
        waterPass.execute = [=](RenderPassContext& ctx) {
            ctx.renderer.SetRenderTarget(ctx.handles.hdrRT, ctx.resources);
            WaterRenderSystem(ctx.scene, ctx.renderer, ctx.resources, ctx.camera,
                              ctx.handles.hdrRT,
                              copyColorShader.IsValid()
                                  ? ctx.resources.GetColorTexture(sceneColorRT, 0)
                                  : renderer::ResourceHandle<renderer::TextureTag>{},
                              core::Time::TotalTime(), &ctx.settings);
        };
        scene.QueueUserRenderPass(std::move(waterPass));
    };

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

    // ── Terrain (フォワードオペーク) ───────────────────────────────────────────
    // ForwardOpaque / GBuffer DepthCopy の後・Sky の前に描画する。
    // WHY: Sky より前に描画することで地形の上に空が被らない。
    //      ForwardOpaque と同じ HDR RT (depth buffer 共有) で描画することで
    //      Player 等の不透明オブジェクトと正しく depth test される。
    //      RenderSystem 内に統合することでポストプロセス（bloom/SSAO等）も適用される。
    graph.AddPass("TerrainForward", { "ShadowMap", "HDR" }, { "HDR" }, [&]() {
        renderer.SetRenderTarget(passHandles.hdrRT, resources);
        TerrainRenderSystem(scene, renderer, resources, camera, passHandles.hdrRT, settings);
    });

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

    queueWaterPasses();
    for (EntityID id : scene.GetEntities<ScriptComponent>()) {
        auto* sc = scene.GetComponent<ScriptComponent>(id);
        auto* go = scene.GetGameObject(id);
        if (!sc || !go)
            continue;
        for (auto& entry : sc->scripts) {
            if (!entry.script || !entry.script->enabled)
                continue;
            entry.script->SetContext(&scene, go);
            entry.script->OnSetupRenderPasses(graph, passCtx);
        }
    }

    appendQueuedUserPasses(UserRenderPassInjectionPoint::AfterOpaque);

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

    // ── Decal + Trail + Particle ──────────────────────────────────────────────
    graph.AddPass("Decal", { "HDR", "DecalDepth" }, { "HDR" }, [&]() {
        ExecuteDecalPass(passCtx);
    });

    graph.AddPass("MeshTrail", { "HDR" }, { "HDR" }, [&]() {
        ExecuteMeshTrailPass(passCtx);
    });

    graph.AddPass("Trail", { "HDR" }, { "HDR" }, [&]() {
        ExecuteTrailPass(passCtx);
    });

    graph.AddPass("Particle", { "HDR" }, { "HDR" }, [&]() {
        ExecuteParticlePass(passCtx);
    });

    appendQueuedUserPasses(UserRenderPassInjectionPoint::AfterTransparent);

    // ── Selection / Debug ─────────────────────────────────────────────────────
    graph.AddPass("UnderwaterCaustics", { "HDR" }, { "HDR" }, [&]() {
        ExecuteCausticsPass(passCtx);
    });

    if (selectionOutlineEnabled) {
        graph.AddPass("SelectionMask", { "HDR" }, { "SelectionMask" }, [&]() {
            ExecuteSelectionMaskPass(passCtx);
        });
    }

    graph.AddPass("DebugColliders", { "HDR" }, { "HDR" }, [&]() {
        ExecuteDebugCollidersPass(passCtx);
    });

    graph.AddPass("ScriptDebugDraw", { "HDR" }, { "HDR" }, [&]() {
        scene.TickScriptDebugDrawCommands(core::Time::DeltaTime());
        renderer::DebugDraw::BeginFrame(passCtx.renderer, passCtx.resources, passCtx.camera.GetViewProjection());

        // OnDrawGizmos: DebugDraw::BeginFrame/Flush の区間内で Script が gizmo プロキシを使って
        // 視野錐・ウェイポイント・検知範囲などを直接描画する。
        // WHY: コマンドキュー経由の ScriptDebugProxy と異なり、Gizmo の複合プリミティブは
        //      レンダリング区間内で直接呼ぶ必要があるため、専用コールバックを設ける。
        for (EntityID id : scene.GetEntities<ScriptComponent>()) {
            auto* sc = scene.GetComponent<ScriptComponent>(id);
            auto* go = scene.GetGameObject(id);
            if (!sc || !go) continue;
            for (auto& entry : sc->scripts) {
                if (!entry.script || !entry.script->enabled) continue;
                entry.script->SetContext(&scene, go);
                entry.script->gizmo.renderer = &passCtx.renderer;
                entry.script->OnDrawGizmos();
                entry.script->gizmo.renderer = nullptr;
            }
        }

        for (const auto& command : scene.GetScriptDebugDrawCommands()) {
            switch (command.type) {
            case ScriptDebugDrawType::Line:
                renderer::DebugDraw::Line(passCtx.renderer, command.a, command.b, command.color);
                break;
            case ScriptDebugDrawType::Sphere:
                renderer::DebugDraw::Sphere(passCtx.renderer, command.a, command.radius, command.color);
                break;
            case ScriptDebugDrawType::Box:
                renderer::DebugDraw::Box(passCtx.renderer, command.a, command.halfExtents, command.color);
                break;
            case ScriptDebugDrawType::Ray:
                renderer::DebugDraw::Line(passCtx.renderer, command.a, command.b, command.color);
                break;
            case ScriptDebugDrawType::Arrow:
                // headLength = radius, headRadius = halfExtents.x
                renderer::DebugDraw::Arrow(passCtx.renderer, command.a, command.b,
                                           command.radius, command.halfExtents.x, command.color);
                break;
            case ScriptDebugDrawType::Cone:
                // direction = b, height = halfExtents.x, baseRadius = radius
                renderer::DebugDraw::Cone(passCtx.renderer, command.a, command.b,
                                          command.halfExtents.x, command.radius, command.color);
                break;
            }
        }
        renderer::DebugDraw::Flush();
    });

    graph.AddPass("DebugDecalBounds", { "HDR" }, { "HDR" }, [&]() {
        ExecuteDecalDebugPass(passCtx);
    });

    appendQueuedUserPasses(UserRenderPassInjectionPoint::BeforePostProcess);

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

    if (uiOptions && uiOptions->enabled) {
        graph.AddPass(
            "UIPass",
            { { "Output", renderer::RenderGraph::ResourceUsage::ReadWrite } },
            [&]() {
                // WHY: UI は最終フレームへの合成であり、Composite / FXAA / CustomPostProcess の
                //      どの分岐が最後に Output を書いたかに依存してはいけない。
                //      Output を ReadWrite する RenderGraph pass として登録し、この pass 内で
                //      明示的に outputRT をバインドすることで、Game / Scene / UI Viewport の
                //      いずれでも同じ順序と同じ RT に描画できる。
                renderer.SetRenderTarget(outputRT, resources);
                const float uiWidth = uiOptions->viewportWidth > 0.0f
                    ? uiOptions->viewportWidth
                    : static_cast<float>(sHdrW);
                const float uiHeight = uiOptions->viewportHeight > 0.0f
                    ? uiOptions->viewportHeight
                    : static_cast<float>(sHdrH);
                UISystem(scene,
                         renderer,
                         resources,
                         uiWidth,
                         uiHeight,
                         uiOptions->mouseInCanvasSpace,
                         uiOptions->mousePressed,
                         camera.GetViewProjection(),
                         uiOptions->targetView);
            });
    }

    for (EntityID id : scene.GetEntities<ScriptComponent>()) {
        auto* sc = scene.GetComponent<ScriptComponent>(id);
        auto* go = scene.GetGameObject(id);
        if (!sc || !go)
            continue;
        for (auto& entry : sc->scripts) {
            if (!entry.script || !entry.script->enabled)
                continue;
            entry.script->SetContext(&scene, go);
            entry.script->OnPreRender();
        }
    }

    // =========================================================================
    // RenderGraph 実行 + デバッグスナップショット更新
    // =========================================================================

    // GPU Timestamp Query の前フレーム結果を収集してからフレームを開始する。
    // WHY: GpuProfCollect を先に呼ぶことで前フレームの非同期クエリが確定している可能性を最大化する。
    renderer.GpuProfCollect();
    renderer.GpuProfBeginFrame();

    // GPU フックを RenderGraph に設定する。CPU フックとは独立しているため、
    // Profiler の CPU スコープ計測と干渉しない。
    graph.SetGpuProfilerHooks(
        [&](std::string_view name) { renderer.GpuProfBeginPass(name.data()); },
        [&](std::string_view name) { renderer.GpuProfEndPass(name.data()); }
    );

    bool graphExecuted = false;
    {
        FBZZ_PROFILE_SCOPE("RenderGraph::Execute");
        graphExecuted = graph.Execute();
    }

    renderer.GpuProfEndFrame();
    assert(graphExecuted);
    (void)graphExecuted;

    for (EntityID id : scene.GetEntities<ScriptComponent>()) {
        auto* sc = scene.GetComponent<ScriptComponent>(id);
        auto* go = scene.GetGameObject(id);
        if (!sc || !go)
            continue;
        for (auto& entry : sc->scripts) {
            if (!entry.script || !entry.script->enabled)
                continue;
            entry.script->SetContext(&scene, go);
            entry.script->OnPostRender();
        }
    }

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

        // GPU 計測結果を Snapshot に詰める。QUERY_LATENCY フレーム以内は空になる。
        for (const auto& gp : renderer.GpuProfGetResults())
            dbgSnap.gpuPassTimings.push_back({ gp.name, gp.gpuMs });

        // カリング統計を Snapshot に詰める
        dbgSnap.renderStats.totalObjects    = passCtx.statsTotalObjects;
        dbgSnap.renderStats.frustumCulled   = passCtx.statsFrustumCulled;
        dbgSnap.renderStats.occlusionCulled = passCtx.statsOcclusionCulled;
        dbgSnap.renderStats.drawCalls       = passCtx.statsDrawCalls;
        dbgSnap.renderStats.vertexCount     = passCtx.statsVertexCount;
        dbgSnap.renderStats.triangleCount   = passCtx.statsTriangleCount;

        renderer::RenderDebugOverlay::UpdateSnapshot(dbgSnap, rs.passViewerEnabled);
    }
}

} // namespace fbzz::scene
