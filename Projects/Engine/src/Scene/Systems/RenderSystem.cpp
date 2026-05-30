// FBZZ Engine
// RenderSystem.cpp | fbzz::scene
// Scene render system entry point
// Builds render graph passes and submits renderer draw calls.
#include "Engine/Scene/Systems/RenderSystem.hpp"
#include "Engine/Renderer/RenderSettings.hpp"
#include "Engine/Renderer/RenderDebugOverlay.hpp"
#include "RenderPasses/DecalPass.hpp"
#include "RenderPasses/DebugPasses.hpp"
#include "RenderPasses/PostProcessPasses.hpp"
#include "RenderPasses/RenderPassContext.hpp"
#include "RenderPasses/SelectionPasses.hpp"
#include "Engine/Core/Logger.hpp"
#include "Engine/Core/Time.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Components/DecalComponent.hpp"
#include "Engine/Scene/Components/LightComponent.hpp"
#include "Engine/Scene/Components/MeshRenderer.hpp"
#include "Engine/Scene/Components/MaterialComponent.hpp"
#include "Engine/Asset/AssetManager.hpp"
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
#include "Engine/Renderer/IShader.hpp"
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
#include <map>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
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

bool IsSurfaceMaterialShader(std::string_view path)
{
    std::string lower(path);
    std::replace(lower.begin(), lower.end(), '\\', '/');
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("/material/surface/") != std::string::npos;
}

bool ShouldRenderGameObject(const GameObject& go, fbzz::LayerMask cullingMask)
{
    // WHY: Hierarchy の非表示は GameObject::activeSelf に集約する。
    //      各描画パスで同じ条件を使い、通常描画・影・選択表示の不一致を防ぐ。
    return go.activeSelf() && fbzz::Layer::Contains(cullingMask, go.layer);
}

// MaterialComponent の blendMode / doubleSided から PipelineState を取得する。
// 同じ組み合わせはキャッシュで再利用し、毎フレームの CreatePipelineState 呼び出しを避ける。
// WHY: DX11 はモノリシック PSO を持たないため、ラスタライザ・ブレンド・深度の
//      組み合わせを手動でキャッシュする必要がある。
static renderer::ResourceHandle<renderer::PipelineStateTag> GetOrCreateMaterialPSO(
    renderer::ResourceManager& resources,
    renderer::BlendMode        blend,
    bool                       doubleSided)
{
    // 両面描画はバックフェースカリングを無効化する。
    const renderer::RasterizerMode raster = doubleSided
        ? renderer::RasterizerMode::SOLID_NOCULL
        : renderer::RasterizerMode::SOLID;
    // 半透明・加算は深度書き込みをオフにし、背後のオブジェクトが透けて見えるようにする。
    const renderer::DepthMode depth = (blend == renderer::BlendMode::OPAQUE)
        ? renderer::DepthMode::DEPTH_ON
        : renderer::DepthMode::DEPTH_READ;

    // WHY: ビットパッキング (旧実装) は enum 値追加時にサイレントなキー衝突が起きるため、
    //      構造体を直接比較する std::map に変更した。
    struct DescLess {
        bool operator()(const renderer::PipelineStateDesc& a,
                        const renderer::PipelineStateDesc& b) const noexcept {
            if (a.rasterizer != b.rasterizer) return a.rasterizer < b.rasterizer;
            if (a.blend      != b.blend)      return a.blend      < b.blend;
            return a.depth < b.depth;
        }
    };
    static std::map<renderer::PipelineStateDesc,
                    renderer::ResourceHandle<renderer::PipelineStateTag>,
                    DescLess> s_cache;
    const renderer::PipelineStateDesc desc{ raster, blend, depth };
    auto it = s_cache.find(desc);
    if (it != s_cache.end()) return it->second;
    auto handle = resources.CreatePipelineState(desc);
    s_cache[desc] = handle;
    return handle;
}

renderer::Material* SyncMaterial(MaterialComponent& mc, renderer::ResourceManager& resources)
{
    if (!mc.enabled) return nullptr;

    if (!mc.material)
        mc.material = std::make_shared<renderer::Material>();

    // WHY: MaterialComponent は自身の Material を排他所有する設計のため、
    //      use_count() による共有検出 (スレッドセーフでない、将来の多参照で誤動作する) は廃止。
    //      Material を共有したい場合は AssetManager 経由のアセット参照として設計する。

    auto& material = *mc.material;
    material.shaderPath = mc.shaderPath;
    material.shader = mc.shaderPath.empty()
        ? renderer::ResourceHandle<renderer::ShaderTag>{}
        : resources.LoadShader(mc.shaderPath);

    // Descriptor を取得してシェーダー切り替え時に paramData を再初期化する。
    const renderer::ShaderDescriptor* desc = nullptr;
    if (auto* shader = resources.Get(material.shader))
        desc = &shader->GetDescriptor();

    if (desc && mc.paramData.size() != desc->cbufferSize)
        mc.InitFromDescriptor(*desc);

    material.paramData = mc.paramData;

    // テクスチャパス → ResourceHandle に解決
    const size_t slotCount = mc.texturePaths.size();
    material.textures.resize(slotCount);
    for (size_t i = 0; i < slotCount; ++i)
    {
        material.textures[i] = mc.texturePaths[i].empty()
            ? renderer::ResourceHandle<renderer::TextureTag>{}
            : resources.LoadTexture(mc.texturePaths[i]);
    }

    static renderer::ShaderDescriptor s_fallback;
    material.Upload(resources, desc ? *desc : s_fallback);
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
    static auto ssaoShader      = resources.LoadShader("assets/shaders/PostProcess/AmbientOcclusion/SSAO.cs.hlsl");
    static auto ssaoBlurShader  = resources.LoadShader("assets/shaders/PostProcess/AmbientOcclusion/SSAOBlur.cs.hlsl");
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
    // FIXME: 以下の static ハンドルはデバイスリセット (フルスクリーン切替・GPU ドライバ更新) 時に
    //        無効化されない。DX11 DeviceRemoved 対応を実装する際はここを全面的に見直す。
    //        ResourceManager に Reset() API を追加し、Application ループから呼び出す設計が必要。
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

    static auto decalShader = resources.LoadShader("assets/shaders/Material/Decal/Decal.hlsl");
    static auto decalPSO    = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_OFF
    });
    static auto decalCB     = resources.CreateConstantBuffer(sizeof(DecalCB));
    static auto decalMaskShader = resources.LoadShader("assets/shaders/Material/Decal/DecalMask.hlsl");
    static auto decalMaskPso    = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE,
        renderer::DepthMode::DEPTH_OFF
    });

    // Deferred pipeline shaders — loaded once and reused across frames.
    static auto gbufferShader          = resources.LoadShader("assets/shaders/Pipeline/Deferred/GBuffer.hlsl");
    static auto deferredLightingShader = resources.LoadShader("assets/shaders/Pipeline/Deferred/DeferredLighting.hlsl");
    static auto depthCopyShader        = resources.LoadShader("assets/shaders/Pipeline/Deferred/DepthCopy.hlsl");

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
    static renderer::ResourceHandle<renderer::RenderTargetTag> customPostProcessRT[2];
    static renderer::ResourceHandle<renderer::RenderTargetTag> gbufferRT;      // Deferred: MRT (color×2 + depth)
    static renderer::ResourceHandle<renderer::RenderTargetTag> decalDepthRT;   // デカール深度読み取り用コピー先
    static renderer::ResourceHandle<renderer::RenderTargetTag> decalMaskRT;    // 除外オブジェクト描画先 (1-color)
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
            gbufferRT       = resources.CreateRenderTarget(curW, curH, 2); // GBuffer0 + GBuffer1
            decalDepthRT    = resources.CreateRenderTarget(curW, curH, 0); // 深度のみ
            decalMaskRT     = resources.CreateRenderTarget(curW, curH, 1); // 除外マスク (1-color)
            bloomHalf       = resources.CreateComputeTexture(std::max(1u, curW / 2), std::max(1u, curH / 2));
            bloomFull       = resources.CreateComputeTexture(curW, curH);
            ssaoRaw         = resources.CreateComputeTexture(curW, curH);
            ssaoBlur        = resources.CreateComputeTexture(curW, curH);
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
    passHandles.customPostProcessRT[0] = customPostProcessRT[0];
    passHandles.customPostProcessRT[1] = customPostProcessRT[1];
    passHandles.gbufferRT = gbufferRT;
    passHandles.bloomHalf = bloomHalf;
    passHandles.bloomFull = bloomFull;
    passHandles.ssaoRaw = ssaoRaw;
    passHandles.ssaoBlur = ssaoBlur;
    passHandles.ssaoShader = ssaoShader;
    passHandles.ssaoBlurShader = ssaoBlurShader;
    passHandles.bloomDownShader = bloomDownShader;
    passHandles.bloomUpShader = bloomUpShader;
    passHandles.compositeShader = compositeShader;
    passHandles.selectionMaskShader = selectionMaskShader;
    passHandles.selectionMaskSkinnedShader = selectionMaskSkinnedShader;
    passHandles.selectionOutlineShader = selectionOutlineShader;
    passHandles.fxaaShader = fxaaShader;
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
    passHandles.selectionMaskPSO = selectionMaskPso;
    passHandles.postprocPSO = postprocPSO;
    passHandles.frameCB = frameCB;
    passHandles.objectCB = objectCB;
    passHandles.lightCB = lightCB;
    passHandles.bindPoseSkinningCB = bindPoseSkinningCB;
    passHandles.postprocCB = postprocCB;
    passHandles.outlineCB = outlineCB;
    passHandles.decalDepthRT    = decalDepthRT;
    passHandles.decalMaskRT     = decalMaskRT;
    passHandles.decalShader     = decalShader;
    passHandles.decalMaskShader = decalMaskShader;
    passHandles.decalPSO        = decalPSO;
    passHandles.decalMaskPSO    = decalMaskPso;
    passHandles.decalCB         = decalCB;

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

    const bool isDeferred = (rs.pipeline == renderer::RenderingPipeline::Deferred);
    const bool ssaoEnabled =
        isDeferred &&
        rs.postProcess.ambientOcclusion.enabled &&
        ssaoShader.IsValid() &&
        ssaoBlurShader.IsValid() &&
        ssaoRaw.IsValid() &&
        ssaoBlur.IsValid();

    renderer::RenderGraph graph;
    graph.DeclareResource("Output", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, true, false });
    graph.DeclareResource("ShadowMap", { renderer::RenderGraph::ResourceKind::RenderTarget, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, 0, false, false });
    graph.DeclareResource("HDR", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    graph.DeclareResource("LDR", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    graph.DeclareResource("SelectionMask", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    graph.DeclareResource("Outline", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    graph.DeclareResource("CustomPostProcess0", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    graph.DeclareResource("CustomPostProcess1", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    graph.DeclareResource("Bloom", { renderer::RenderGraph::ResourceKind::Texture, sHdrW, sHdrH, 0, false, true });
    if (isDeferred)
        graph.DeclareResource("GBuffer", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, false });
    if (ssaoEnabled)
        graph.DeclareResource("SSAO", { renderer::RenderGraph::ResourceKind::Texture, sHdrW, sHdrH, 0, false, true });
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
            if (!ShouldRenderGameObject(go, cullingMask)) continue;
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
                if (!ShouldRenderGameObject(go, cullingMask)) continue;
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

    // =========================================================================
    // Forward pipeline: opaque static + skinned meshes in one pass.
    // =========================================================================
    if (!isDeferred) {

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

    // 透明オブジェクトを収集し far→near でソートしてから Submit する。
    // WHY: 不透明が先にデプスバッファを確立することで、半透明オブジェクトが
    //      不透明オブジェクトに正しくオクルードされる。半透明同士は背面から前面へ
    //      ソートすることで、加算ではなくアルファブレンドのオブジェクトが正しく重なる。
    // WHY: renderQueue でグループを決め、同一 Queue 内では far→near ソートする。
    //      これにより「エフェクト (Queue=2000) は常にキャラ (Queue=0) より後に描く」が実現できる。
    struct TransparentEntry {
        renderer::DrawCall dc;
        PerObjectCB        objData;
        float              distSqFromCamera;
        int32_t            renderQueue;
    };
    std::vector<TransparentEntry> transparentQueue;

    // 不透明 static meshes → 即座に Submit
    for (auto& go : scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, cullingMask)) continue;
        auto* mr  = go.GetComponent<MeshRenderer>();
        auto* mat = go.GetComponent<MaterialComponent>();
        if (!mr || !mr->enabled || !mr->mesh || !mat) continue;
        if (!mr->mesh->vertexBuffer.IsValid() || !mr->mesh->indexBuffer.IsValid()) continue;
        if (mr->mesh->isSkinned) continue;
        if (mat->blendMode != renderer::BlendMode::OPAQUE) continue;
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
        dc.pipelineState      = rs.wireframeMode ? wireframePso : GetOrCreateMaterialPSO(resources, mat->blendMode, mat->doubleSided);
        dc.layer              = renderer::RenderLayer::OPAQUE;
        dc.constantBuffers[0] = frameCB;
        dc.constantBuffers[1] = objectCB;
        dc.constantBuffers[2] = material->paramsBuffer;
        dc.constantBuffers[3] = lightCB;
        dc.constantBuffers[4] = shadowCB;
        for (size_t ti = 0; ti < material->textures.size() && ti < 5; ++ti)
            if (material->textures[ti].IsValid()) dc.textures[ti] = material->textures[ti];
        dc.textures[8] = passHandles.shadowDepthTex;
        renderer.Submit(dc, resources);
    }

    // 不透明 skinned meshes → 即座に Submit
    for (auto& go : scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, cullingMask)) continue;
        auto* smr  = go.GetComponent<SkinnedMeshRenderer>();
        auto* mat  = go.GetComponent<MaterialComponent>();
        auto* anim = go.GetComponent<AnimatorComponent>();
        if (!smr || !smr->enabled || !smr->model) continue;
        if (!mat) continue;
        if (mat->blendMode != renderer::BlendMode::OPAQUE) continue;
        auto* material = SyncMaterial(*mat, resources);
        if (!material) continue;
        // WHY: Surface 版シェーダー (Material/Surface/) はスキニング処理を持たない VS を使用する。
        //      SkinnedMeshRenderer に誤って設定するとメッシュが破綻するため SkinnedPBR へフォールバックし、
        //      開発者がすぐ気づけるよう警告を出す。
        const bool surfaceShaderMisassigned =
            material->shader.IsValid() && IsSurfaceMaterialShader(material->shaderPath);
        if (surfaceShaderMisassigned)
        {
            FBZZ_LOG_WARN("SkinnedMeshRenderer に Surface シェーダーが設定されています: %s"
                          " → SkinnedPBR にフォールバック。Skinned/ 以下のシェーダーを使用してください。",
                          material->shaderPath.c_str());
        }
        const auto skinnedShader = (!surfaceShaderMisassigned && material->shader.IsValid())
            ? material->shader : skinnedPbrShader;
        if (!skinnedShader.IsValid()) continue;

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
            dc.shader             = skinnedShader;
            dc.pipelineState      = rs.wireframeMode ? wireframePso : GetOrCreateMaterialPSO(resources, mat->blendMode, mat->doubleSided);
            dc.layer              = renderer::RenderLayer::OPAQUE;
            dc.constantBuffers[0] = frameCB;
            dc.constantBuffers[1] = objectCB;
            dc.constantBuffers[2] = material->paramsBuffer;
            dc.constantBuffers[3] = lightCB;
            dc.constantBuffers[4] = shadowCB;
            dc.constantBuffers[7] = skinCB;
            for (size_t ti = 0; ti < material->textures.size() && ti < 5; ++ti)
                if (material->textures[ti].IsValid()) dc.textures[ti] = material->textures[ti];
            dc.textures[8] = passHandles.shadowDepthTex;
            renderer.Submit(dc, resources);
        }
    }

    // 半透明 static meshes → transparentQueue に蓄積 (Submit しない)
    for (auto& go : scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, cullingMask)) continue;
        auto* mr  = go.GetComponent<MeshRenderer>();
        auto* mat = go.GetComponent<MaterialComponent>();
        if (!mr || !mr->enabled || !mr->mesh || !mat) continue;
        if (!mr->mesh->vertexBuffer.IsValid() || !mr->mesh->indexBuffer.IsValid()) continue;
        if (mr->mesh->isSkinned) continue;
        if (mat->blendMode == renderer::BlendMode::OPAQUE) continue;
        auto* material = SyncMaterial(*mat, resources);
        if (!material || !material->shader.IsValid()) continue;

        PerObjectCB objData{};
        objData.world             = go.transform.GetWorldMatrix();
        objData.worldInvTranspose = math::Matrix4::Transpose(math::Matrix4::Inverse(objData.world));

        renderer::DrawCall dc;
        dc.vertexBuffer       = mr->mesh->vertexBuffer;
        dc.indexBuffer        = mr->mesh->indexBuffer;
        dc.indexCount         = mr->mesh->indexCount;
        dc.vertexCount        = mr->mesh->vertexCount;
        dc.shader             = material->shader;
        dc.pipelineState      = rs.wireframeMode ? wireframePso : GetOrCreateMaterialPSO(resources, mat->blendMode, mat->doubleSided);
        dc.layer              = renderer::RenderLayer::TRANSPARENT;
        dc.constantBuffers[0] = frameCB;
        dc.constantBuffers[1] = objectCB;
        dc.constantBuffers[2] = material->paramsBuffer;
        dc.constantBuffers[3] = lightCB;
        dc.constantBuffers[4] = shadowCB;
        for (size_t ti = 0; ti < material->textures.size() && ti < 5; ++ti)
            if (material->textures[ti].IsValid()) dc.textures[ti] = material->textures[ti];
        dc.textures[8] = passHandles.shadowDepthTex;

        const float dx = objData.world.m[0][3] - camera.m_position.x;
        const float dy = objData.world.m[1][3] - camera.m_position.y;
        const float dz = objData.world.m[2][3] - camera.m_position.z;
        transparentQueue.push_back({ dc, objData, dx*dx + dy*dy + dz*dz, mat->renderQueue });
    }

    // 半透明 skinned meshes → transparentQueue に蓄積 (Submit しない)
    for (auto& go : scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, cullingMask)) continue;
        auto* smr  = go.GetComponent<SkinnedMeshRenderer>();
        auto* mat  = go.GetComponent<MaterialComponent>();
        auto* anim = go.GetComponent<AnimatorComponent>();
        if (!smr || !smr->enabled || !smr->model) continue;
        if (!mat) continue;
        if (mat->blendMode == renderer::BlendMode::OPAQUE) continue;
        auto* material = SyncMaterial(*mat, resources);
        if (!material) continue;
        const bool surfaceShaderMisassigned =
            material->shader.IsValid() && IsSurfaceMaterialShader(material->shaderPath);
        if (surfaceShaderMisassigned)
        {
            FBZZ_LOG_WARN("SkinnedMeshRenderer に Surface シェーダーが設定されています: %s"
                          " → SkinnedPBR にフォールバック。Skinned/ 以下のシェーダーを使用してください。",
                          material->shaderPath.c_str());
        }
        const auto skinnedShader = (!surfaceShaderMisassigned && material->shader.IsValid())
            ? material->shader : skinnedPbrShader;
        if (!skinnedShader.IsValid()) continue;

        PerObjectCB objData{};
        objData.world             = go.transform.GetWorldMatrix();
        objData.worldInvTranspose = math::Matrix4::Transpose(math::Matrix4::Inverse(objData.world));

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
            dc.shader             = skinnedShader;
            dc.pipelineState      = rs.wireframeMode ? wireframePso : GetOrCreateMaterialPSO(resources, mat->blendMode, mat->doubleSided);
            dc.layer              = renderer::RenderLayer::TRANSPARENT;
            dc.constantBuffers[0] = frameCB;
            dc.constantBuffers[1] = objectCB;
            dc.constantBuffers[2] = material->paramsBuffer;
            dc.constantBuffers[3] = lightCB;
            dc.constantBuffers[4] = shadowCB;
            dc.constantBuffers[7] = skinCB;
            for (size_t ti = 0; ti < material->textures.size() && ti < 5; ++ti)
                if (material->textures[ti].IsValid()) dc.textures[ti] = material->textures[ti];
            dc.textures[8] = passHandles.shadowDepthTex;

            const float dx = objData.world.m[0][3] - camera.m_position.x;
            const float dy = objData.world.m[1][3] - camera.m_position.y;
            const float dz = objData.world.m[2][3] - camera.m_position.z;
            transparentQueue.push_back({ dc, objData, dx*dx + dy*dy + dz*dz, mat->renderQueue });
        }
    }

    // far→near (distSqFromCamera 降順) でソートしてから Submit する。
    std::sort(transparentQueue.begin(), transparentQueue.end(),
        [](const TransparentEntry& a, const TransparentEntry& b) {
            // renderQueue が異なれば小さい方を先に描画する。同一 Queue 内は far→near。
            if (a.renderQueue != b.renderQueue) return a.renderQueue < b.renderQueue;
            return a.distSqFromCamera > b.distSqFromCamera;
        });
    for (auto& entry : transparentQueue) {
        resources.Update(objectCB, &entry.objData, sizeof(PerObjectCB));
        renderer.Submit(entry.dc, resources);
    }

    }); // ForwardOpaque

    } // if (!isDeferred)

    // =========================================================================
    // Deferred pipeline:
    //   GBuffer → DepthCopy → Sky → DeferredLighting → DeferredSkinnedForward
    //
    // パス実行順:
    //   1. GBuffer: 静的メッシュをジオメトリバッファへ書き込む
    //   2. DepthCopy: GBuffer 深度を hdrRT 深度へ転写し、Sky / スキンドが正しく深度テストできるようにする
    //   3. Sky: hdrRT 深度 == 1.0 の背景部分にのみ描画 (DEPTH_SKY)
    //   4. DeferredLighting: フルスクリーンで PBR ライティングを適用。gbuf_depth==1.0 は discard して Sky 色を保持
    //   5. DeferredSkinnedForward: スキンドメッシュをフォワードで描画 (hdrRT の GBuffer 深度を利用)
    // =========================================================================
    if (isDeferred) {

    graph.AddPass("DeferredGBuffer", { "ShadowMap" }, { "GBuffer" }, [&]() {
    renderer.SetRenderTarget(gbufferRT, resources);
    renderer.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });

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

    if (gbufferShader.IsValid()) {
        for (auto& go : scene.GameObjects()) {
            if (!ShouldRenderGameObject(go, cullingMask)) continue;
            auto* mr  = go.GetComponent<MeshRenderer>();
            auto* mat = go.GetComponent<MaterialComponent>();
            if (!mr || !mr->enabled || !mr->mesh || !mat) continue;
            if (!mr->mesh->vertexBuffer.IsValid() || !mr->mesh->indexBuffer.IsValid()) continue;
            if (mr->mesh->isSkinned) continue;
            // 半透明・加算マテリアルは GBuffer に書き込まない。フォワードパスで描画する。
            // WHY: GBuffer はアルファブレンドをサポートしない (MRT への書き込みが 1 つの値のため)。
            if (mat->blendMode != renderer::BlendMode::OPAQUE) continue;
            auto* material = SyncMaterial(*mat, resources);

            PerObjectCB objData{};
            objData.world             = go.transform.GetWorldMatrix();
            objData.worldInvTranspose = math::Matrix4::Transpose(math::Matrix4::Inverse(objData.world));
            resources.Update(objectCB, &objData, sizeof(PerObjectCB));

            renderer::DrawCall dc;
            dc.vertexBuffer       = mr->mesh->vertexBuffer;
            dc.indexBuffer        = mr->mesh->indexBuffer;
            dc.indexCount         = mr->mesh->indexCount;
            dc.vertexCount        = mr->mesh->vertexCount;
            dc.shader             = gbufferShader;
            dc.pipelineState      = rs.wireframeMode ? wireframePso : pso;
            dc.constantBuffers[0] = frameCB;
            dc.constantBuffers[1] = objectCB;
            dc.constantBuffers[2] = material ? material->paramsBuffer : renderer::ResourceHandle<renderer::ConstantBufferTag>{};
            if (material)
                for (size_t ti = 0; ti < material->textures.size() && ti < 5; ++ti)
                    if (material->textures[ti].IsValid()) dc.textures[ti] = material->textures[ti];
            renderer.Submit(dc, resources);
        }
    }
    }); // DeferredGBuffer

    graph.AddPass("DeferredDepthCopy", { "GBuffer" }, { "HDR" }, [&]() {
    // hdrRT をクリア (カラー・深度 1.0 にリセット) してから GBuffer 深度を転写する。
    // この深度は Sky (DEPTH_SKY) と DeferredSkinnedForward (DEPTH_ON) が参照する。
    renderer.SetRenderTarget(hdrRT, resources);
    renderer.Clear({ 0.0f, 0.0f, 0.0f, 1.0f });

    if (depthCopyShader.IsValid() && gbufferRT.IsValid()) {
        renderer::DrawCall dc;
        dc.shader        = depthCopyShader;
        dc.pipelineState = pso;  // DEPTH_ON: 深度テスト + 書き込みあり
        dc.vertexCount   = 3;
        dc.textures[7]   = resources.GetDepthTexture(gbufferRT);  // TEX_DEPTH
        renderer.Submit(dc, resources);
    }
    }); // DeferredDepthCopy

    } // if (isDeferred)

    graph.AddPass("Sky", { "HDR" }, { "HDR" }, [&]() {
    if (skydomeShader.IsValid() && skydomeMesh && skydomeMesh->vertexBuffer.IsValid() && skydomeMesh->indexBuffer.IsValid())
    {
        for (auto& go : scene.GameObjects()) {
            if (!ShouldRenderGameObject(go, cullingMask)) continue;
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

    }); // Sky

    if (isDeferred) {

    if (ssaoEnabled) {
        graph.AddPass("SSAO", { "GBuffer" }, { "SSAO" }, [&]() {
            ExecuteSSAOPass(passCtx);
        });
    }

    auto executeDeferredLighting = [&]() {
    // Sky 色・深度を保持したまま上書きするため SetRenderTarget のみ (Clear しない)。
    renderer.SetRenderTarget(hdrRT, resources);
    renderer.SetSampler(0, renderer::SamplerMode::WRAP_ANISOTROPIC);
    renderer.SetSampler(1, renderer::SamplerMode::BORDER_ZERO);

    PostProcCB lightingPostData{};
    lightingPostData.ssaoIntensity = ssaoEnabled
        ? rs.postProcess.ambientOcclusion.intensity
        : 0.0f;
    resources.Update(postprocCB, &lightingPostData, sizeof(PostProcCB));

    if (deferredLightingShader.IsValid() && gbufferRT.IsValid()) {
        renderer::DrawCall dc;
        dc.shader             = deferredLightingShader;
        dc.pipelineState      = postprocPSO;  // DEPTH_OFF: 深度テスト・書き込みなし
        dc.vertexCount        = 3;
        dc.constantBuffers[0] = frameCB;
        dc.constantBuffers[3] = lightCB;
        dc.constantBuffers[4] = shadowCB;
        dc.textures[5]        = resources.GetColorTexture(gbufferRT, 0);  // TEX_GBUFFER0
        dc.textures[6]        = resources.GetColorTexture(gbufferRT, 1);  // TEX_GBUFFER1
        dc.textures[7]        = resources.GetDepthTexture(gbufferRT);     // TEX_DEPTH
        dc.textures[8]        = resources.GetDepthTexture(shadowMapRT);   // TEX_SHADOW
        dc.textures[9]        = ssaoEnabled
            ? ssaoBlur
            : renderer::ResourceHandle<renderer::TextureTag>{};           // TEX_SSAO
        dc.constantBuffers[5] = postprocCB;
        renderer.Submit(dc, resources);
    }
    };

    if (ssaoEnabled) {
        graph.AddPass("DeferredLighting", { "GBuffer", "HDR", "SSAO" }, { "HDR" }, executeDeferredLighting);
    } else {
        graph.AddPass("DeferredLighting", { "GBuffer", "HDR" }, { "HDR" }, executeDeferredLighting);
    }

    graph.AddPass("DeferredSkinnedForward", { "HDR" }, { "HDR" }, [&]() {
    // スキンドメッシュはフォワードパスで描画する。
    // hdrRT の深度には DeferredDepthCopy で転写した GBuffer 深度が入っているため
    // 静的ジオメトリとの正しいオクルージョンが保たれる。
    renderer.SetRenderTarget(hdrRT, resources);
    renderer.SetSampler(0, renderer::SamplerMode::WRAP_ANISOTROPIC);
    renderer.SetSampler(1, renderer::SamplerMode::BORDER_ZERO);

    const auto shadowDepthTex = resources.GetDepthTexture(shadowMapRT);

    // 透明オブジェクトを収集し far→near でソートしてから Submit する。
    // WHY: 不透明が先にデプスバッファを確立することで、半透明オブジェクトが
    //      不透明オブジェクトに正しくオクルードされる。半透明同士は背面から前面へ
    //      ソートすることで、加算ではなくアルファブレンドのオブジェクトが正しく重なる。
    // WHY: renderQueue でグループを決め、同一 Queue 内では far→near ソートする。
    //      これにより「エフェクト (Queue=2000) は常にキャラ (Queue=0) より後に描く」が実現できる。
    struct TransparentEntry {
        renderer::DrawCall dc;
        PerObjectCB        objData;
        float              distSqFromCamera;
        int32_t            renderQueue;
    };
    std::vector<TransparentEntry> transparentQueue;

    // 不透明 skinned meshes → 即座に Submit
    for (auto& go : scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, cullingMask)) continue;
        auto* smr  = go.GetComponent<SkinnedMeshRenderer>();
        auto* mat  = go.GetComponent<MaterialComponent>();
        auto* anim = go.GetComponent<AnimatorComponent>();
        if (!smr || !smr->enabled || !smr->model) continue;
        if (!mat) continue;
        if (mat->blendMode != renderer::BlendMode::OPAQUE) continue;
        auto* material = SyncMaterial(*mat, resources);
        if (!material) continue;
        // WHY: Surface 版シェーダー (Material/Surface/) はスキニング処理を持たない VS を使用する。
        //      SkinnedMeshRenderer に誤って設定するとメッシュが破綻するため SkinnedPBR へフォールバックし、
        //      開発者がすぐ気づけるよう警告を出す。
        const bool surfaceShaderMisassigned =
            material->shader.IsValid() && IsSurfaceMaterialShader(material->shaderPath);
        if (surfaceShaderMisassigned)
        {
            FBZZ_LOG_WARN("SkinnedMeshRenderer に Surface シェーダーが設定されています: %s"
                          " → SkinnedPBR にフォールバック。Skinned/ 以下のシェーダーを使用してください。",
                          material->shaderPath.c_str());
        }
        const auto skinnedShader = (!surfaceShaderMisassigned && material->shader.IsValid())
            ? material->shader : skinnedPbrShader;
        if (!skinnedShader.IsValid()) continue;

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
            dc.shader             = skinnedShader;
            dc.pipelineState      = rs.wireframeMode ? wireframePso : GetOrCreateMaterialPSO(resources, mat->blendMode, mat->doubleSided);
            dc.layer              = renderer::RenderLayer::OPAQUE;
            dc.constantBuffers[0] = frameCB;
            dc.constantBuffers[1] = objectCB;
            dc.constantBuffers[2] = material->paramsBuffer;
            dc.constantBuffers[3] = lightCB;
            dc.constantBuffers[4] = shadowCB;
            dc.constantBuffers[7] = skinCB;
            for (size_t ti = 0; ti < material->textures.size() && ti < 5; ++ti)
                if (material->textures[ti].IsValid()) dc.textures[ti] = material->textures[ti];
            dc.textures[8] = shadowDepthTex;
            renderer.Submit(dc, resources);
        }
    }

    // 半透明 skinned meshes → transparentQueue に蓄積 (Submit しない)
    for (auto& go : scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, cullingMask)) continue;
        auto* smr  = go.GetComponent<SkinnedMeshRenderer>();
        auto* mat  = go.GetComponent<MaterialComponent>();
        auto* anim = go.GetComponent<AnimatorComponent>();
        if (!smr || !smr->enabled || !smr->model) continue;
        if (!mat) continue;
        if (mat->blendMode == renderer::BlendMode::OPAQUE) continue;
        auto* material = SyncMaterial(*mat, resources);
        if (!material) continue;
        const bool surfaceShaderMisassigned =
            material->shader.IsValid() && IsSurfaceMaterialShader(material->shaderPath);
        if (surfaceShaderMisassigned)
        {
            FBZZ_LOG_WARN("SkinnedMeshRenderer に Surface シェーダーが設定されています: %s"
                          " → SkinnedPBR にフォールバック。Skinned/ 以下のシェーダーを使用してください。",
                          material->shaderPath.c_str());
        }
        const auto skinnedShader = (!surfaceShaderMisassigned && material->shader.IsValid())
            ? material->shader : skinnedPbrShader;
        if (!skinnedShader.IsValid()) continue;

        PerObjectCB objData{};
        objData.world             = go.transform.GetWorldMatrix();
        objData.worldInvTranspose = math::Matrix4::Transpose(math::Matrix4::Inverse(objData.world));

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
            dc.shader             = skinnedShader;
            dc.pipelineState      = rs.wireframeMode ? wireframePso : GetOrCreateMaterialPSO(resources, mat->blendMode, mat->doubleSided);
            dc.layer              = renderer::RenderLayer::TRANSPARENT;
            dc.constantBuffers[0] = frameCB;
            dc.constantBuffers[1] = objectCB;
            dc.constantBuffers[2] = material->paramsBuffer;
            dc.constantBuffers[3] = lightCB;
            dc.constantBuffers[4] = shadowCB;
            dc.constantBuffers[7] = skinCB;
            for (size_t ti = 0; ti < material->textures.size() && ti < 5; ++ti)
                if (material->textures[ti].IsValid()) dc.textures[ti] = material->textures[ti];
            dc.textures[8] = shadowDepthTex;

            const float dx = objData.world.m[0][3] - camera.m_position.x;
            const float dy = objData.world.m[1][3] - camera.m_position.y;
            const float dz = objData.world.m[2][3] - camera.m_position.z;
            transparentQueue.push_back({ dc, objData, dx*dx + dy*dy + dz*dz, mat->renderQueue });
        }
    }

    // far→near (distSqFromCamera 降順) でソートしてから Submit する。
    std::sort(transparentQueue.begin(), transparentQueue.end(),
        [](const TransparentEntry& a, const TransparentEntry& b) {
            // renderQueue が異なれば小さい方を先に描画する。同一 Queue 内は far→near。
            if (a.renderQueue != b.renderQueue) return a.renderQueue < b.renderQueue;
            return a.distSqFromCamera > b.distSqFromCamera;
        });
    for (auto& entry : transparentQueue) {
        resources.Update(objectCB, &entry.objData, sizeof(PerObjectCB));
        renderer.Submit(entry.dc, resources);
    }
    }); // DeferredSkinnedForward

    graph.AddPass("DeferredForwardTransparent", { "HDR" }, { "HDR" }, [&]() {
    // 透明 Static Mesh を Deferred パイプラインのフォワードパスで描画する。
    // DeferredDepthCopy で転写済みの GBuffer 深度を利用するため、静的不透明ジオメトリとの
    // 正しいオクルージョンが保たれる。
    // WHY: GBuffer はアルファブレンドを扱えないため、透明 Static Mesh は
    //      Deferred ライティング後に別途フォワードで描画する必要がある。
    renderer.SetRenderTarget(hdrRT, resources);
    renderer.SetSampler(0, renderer::SamplerMode::WRAP_ANISOTROPIC);
    renderer.SetSampler(1, renderer::SamplerMode::BORDER_ZERO);

    // WHY: renderQueue でグループを決め、同一 Queue 内では far→near ソートする。
    //      これにより「エフェクト (Queue=2000) は常にキャラ (Queue=0) より後に描く」が実現できる。
    struct TransparentEntry {
        renderer::DrawCall dc;
        PerObjectCB        objData;
        float              distSqFromCamera;
        int32_t            renderQueue;
    };
    std::vector<TransparentEntry> transparentQueue;

    const auto shadowDepthTex = resources.GetDepthTexture(shadowMapRT);

    for (auto& go : scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, cullingMask)) continue;
        auto* mr  = go.GetComponent<MeshRenderer>();
        auto* mat = go.GetComponent<MaterialComponent>();
        if (!mr || !mr->enabled || !mr->mesh || !mat) continue;
        if (!mr->mesh->vertexBuffer.IsValid() || !mr->mesh->indexBuffer.IsValid()) continue;
        if (mr->mesh->isSkinned) continue;
        if (mat->blendMode == renderer::BlendMode::OPAQUE) continue;  // 透明のみ

        auto* material = SyncMaterial(*mat, resources);
        if (!material || !material->shader.IsValid()) continue;

        PerObjectCB objData{};
        objData.world             = go.transform.GetWorldMatrix();
        objData.worldInvTranspose = math::Matrix4::Transpose(math::Matrix4::Inverse(objData.world));

        renderer::DrawCall dc;
        dc.vertexBuffer       = mr->mesh->vertexBuffer;
        dc.indexBuffer        = mr->mesh->indexBuffer;
        dc.indexCount         = mr->mesh->indexCount;
        dc.vertexCount        = mr->mesh->vertexCount;
        dc.shader             = material->shader;
        dc.pipelineState      = rs.wireframeMode ? wireframePso : GetOrCreateMaterialPSO(resources, mat->blendMode, mat->doubleSided);
        dc.layer              = renderer::RenderLayer::TRANSPARENT;
        dc.constantBuffers[0] = frameCB;
        dc.constantBuffers[1] = objectCB;
        dc.constantBuffers[2] = material->paramsBuffer;
        dc.constantBuffers[3] = lightCB;
        dc.constantBuffers[4] = shadowCB;
        for (size_t ti = 0; ti < material->textures.size() && ti < 5; ++ti)
            if (material->textures[ti].IsValid()) dc.textures[ti] = material->textures[ti];
        dc.textures[8] = shadowDepthTex;

        const float dx = objData.world.m[0][3] - camera.m_position.x;
        const float dy = objData.world.m[1][3] - camera.m_position.y;
        const float dz = objData.world.m[2][3] - camera.m_position.z;
        transparentQueue.push_back({ dc, objData, dx*dx + dy*dy + dz*dz, mat->renderQueue });
    }

    std::sort(transparentQueue.begin(), transparentQueue.end(),
        [](const TransparentEntry& a, const TransparentEntry& b) {
            // renderQueue が異なれば小さい方を先に描画する。同一 Queue 内は far→near。
            if (a.renderQueue != b.renderQueue) return a.renderQueue < b.renderQueue;
            return a.distSqFromCamera > b.distSqFromCamera;
        });
    for (auto& entry : transparentQueue) {
        resources.Update(objectCB, &entry.objData, sizeof(PerObjectCB));
        renderer.Submit(entry.dc, resources);
    }
    }); // DeferredForwardTransparent

    } // if (isDeferred)

    // デカール用深度スナップショット — hdrRT の DSV/SRV 競合を回避するため
    // 別の深度専用 RT へコピーしてからデカールが読み取る。
    // Forward: hdrRT 深度をコピー  / Deferred: gbufferRT 深度をコピー
    graph.DeclareResource("DecalDepth", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    graph.AddPass("DecalDepthCopy", { isDeferred ? "GBuffer" : "HDR" }, { "DecalDepth" }, [&]() {
        renderer.SetRenderTarget(decalDepthRT, resources);
        renderer.ClearDepth();
        if (depthCopyShader.IsValid()) {
            renderer::DrawCall dc;
            dc.shader        = depthCopyShader;
            dc.pipelineState = pso;   // DEPTH_ON: 深度書き込みあり
            dc.vertexCount   = 3;
            dc.textures[7]   = isDeferred
                ? resources.GetDepthTexture(gbufferRT)
                : resources.GetDepthTexture(hdrRT);
            renderer.Submit(dc, resources);
        }
    });

    graph.AddPass("Decal", { "HDR", "DecalDepth" }, { "HDR" }, [&]() {
        ExecuteDecalPass(passCtx);
    });

    graph.AddPass("Particle", { "HDR" }, { "HDR" }, [&]() {
    if (particleShader.IsValid() && particleVB.IsValid() && particleIB.IsValid())
    {
        const float dt = core::Time::DeltaTime();

        for (auto& go : scene.GameObjects()) {
            if (!ShouldRenderGameObject(go, cullingMask)) continue;
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

    graph.AddPass("DebugDecalBounds", { "HDR" }, { "HDR" }, [&]() {
        ExecuteDecalDebugPass(passCtx);
    });

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
            const bool writesOutput = lastCustomPass && !rs.postProcess.fxaaEnabled && !selectionOutlineEnabled;
            const uint32_t outputIndex = writesOutput ? 2u : (passIndex % 2u);
            const std::string outputResource = writesOutput
                ? std::string("Output")
                : std::string(outputIndex == 0 ? "CustomPostProcess0" : "CustomPostProcess1");
            const std::string passName = "CustomPostProcess" + std::to_string(passIndex);
            const uint32_t customIndex = customPostProcessIndices[passIndex];

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
            [&]() {
            ExecuteSelectionOutlinePass(passCtx);
        });
    }

    if (rs.postProcess.fxaaEnabled) {
        if (selectionOutlineEnabled) {
            graph.AddPass("FXAA", { "Outline" }, { "Output" }, [&]() {
                ExecuteFxaaPass(passCtx);
            });
        } else if (customPostProcessEnabled) {
            graph.AddPass("FXAA", { std::string_view(postCustomResource) }, { "Output" }, [&]() {
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

    // パスビューア用スナップショットを更新する。
    // GetImTextureID は DX11 ステートに副作用を持つ可能性があるため、GPU 実行中は
    // ハンドルの保存のみ行い、実際の Draw は ImGui フレーム内 (RenderPanels 等) で行う。
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
