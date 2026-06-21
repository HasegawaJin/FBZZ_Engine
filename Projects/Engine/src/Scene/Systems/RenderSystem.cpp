// FBZZ Engine
// RenderSystem.cpp | fbzz::scene
// Scene から DrawCall を生成するオーケストレーター
// 各描画パスの実装は RenderPasses/ 以下の Execute*Pass 関数に委譲する。
#include "Engine/Scene/Systems/RenderSystem.hpp"
#include "Engine/Scene/Systems/RenderPasses/Geometry/TerrainRenderPass.hpp"
#include "Engine/Scene/Systems/RenderPasses/Geometry/WaterRenderPass.hpp"
#include "Engine/Scene/Systems/RenderPasses/Geometry/DetailRenderPass.hpp"
#include "Engine/Scene/Systems/RenderPasses/Geometry/FoliageRenderPass.hpp"
#include "Engine/Scene/Systems/RenderPasses/Geometry/MeshTrailRenderPass.hpp"
#include "Engine/Scene/Systems/RenderPasses/Geometry/TrailRenderPass.hpp"
#include "Engine/Renderer/RenderSettings.hpp"
#include "Engine/Renderer/RenderDebugOverlay.hpp"
#include "Engine/Renderer/DebugDraw.hpp"
#include "RenderPasses/Debug/DebugPasses.hpp"
#include <Physics/World.hpp>
#include "RenderPasses/Geometry/GeometryPasses.hpp"
#include "RenderPasses/PostProcess/PostProcessPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include "RenderPasses/Debug/SelectionPasses.hpp"
#include "Engine/Core/Time.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/ScriptComponent.hpp"
#include "Engine/Scene/Transform.hpp"
#include "Engine/Scene/Components/LightComponent.hpp"
#include "Engine/Scene/Components/MeshRenderer.hpp"
#include "Engine/Scene/Components/SkinnedMeshRenderer.hpp"
#include "Engine/Scene/Components/TerrainComponent.hpp"
#include "Engine/Asset/AssetManager.hpp"
#include "Engine/Renderer/IRenderer.hpp"
#include "Engine/Renderer/Camera.hpp"
#include "Engine/Renderer/LightSystem.hpp"
#include "Engine/Renderer/Mesh.hpp"
#include "Engine/Renderer/PrimitiveMesh.hpp"
#include "Engine/Scene/Systems/RenderPasses/RenderPipeline.hpp"
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
#include <unordered_map>
#include <utility>
#include <vector>

namespace fbzz::scene {

namespace {

struct SceneShadowBounds {
    math::Vector3 center = math::Vector3::ZERO;
    float radius = 0.0f;
    bool valid = false;
};

// Viewport ごとに解像度依存の中間リソースを保持する。
// WHY: Scene View と Game View は解像度が異なるため、単一の static RT 群を共有すると
//      1 フレーム内で互いのサイズへリサイズし続け、D3D11 リソース生成待ちが発生する。
struct ViewRenderTargets {
    renderer::ResourceHandle<renderer::RenderTargetTag> hdr;
    renderer::ResourceHandle<renderer::RenderTargetTag> ldr;
    renderer::ResourceHandle<renderer::RenderTargetTag> selectionMask;
    renderer::ResourceHandle<renderer::RenderTargetTag> outline;
    renderer::ResourceHandle<renderer::RenderTargetTag> customPostProcess[2];
    renderer::ResourceHandle<renderer::RenderTargetTag> gbuffer;
    renderer::ResourceHandle<renderer::RenderTargetTag> decalDepth;
    renderer::ResourceHandle<renderer::RenderTargetTag> decalMask;
    renderer::ResourceHandle<renderer::TextureTag> bloomHalf;
    renderer::ResourceHandle<renderer::TextureTag> bloomFull;
    renderer::ResourceHandle<renderer::TextureTag> ssaoRaw;
    renderer::ResourceHandle<renderer::TextureTag> ssaoBlur;
    uint32_t width = 0;
    uint32_t height = 0;
};

// Resize 前のネイティブリソースを ResourceManager から確実に解放する。
void ReleaseViewRenderTargets(ViewRenderTargets& targets, renderer::ResourceManager& resources)
{
    if (targets.hdr.IsValid())                  resources.Release(targets.hdr);
    if (targets.ldr.IsValid())                  resources.Release(targets.ldr);
    if (targets.selectionMask.IsValid())        resources.Release(targets.selectionMask);
    if (targets.outline.IsValid())              resources.Release(targets.outline);
    if (targets.customPostProcess[0].IsValid()) resources.Release(targets.customPostProcess[0]);
    if (targets.customPostProcess[1].IsValid()) resources.Release(targets.customPostProcess[1]);
    if (targets.gbuffer.IsValid())              resources.Release(targets.gbuffer);
    if (targets.decalDepth.IsValid())           resources.Release(targets.decalDepth);
    if (targets.decalMask.IsValid())            resources.Release(targets.decalMask);
    if (targets.bloomHalf.IsValid())            resources.Release(targets.bloomHalf);
    if (targets.bloomFull.IsValid())            resources.Release(targets.bloomFull);
    if (targets.ssaoRaw.IsValid())              resources.Release(targets.ssaoRaw);
    if (targets.ssaoBlur.IsValid())             resources.Release(targets.ssaoBlur);
    targets = {};
}

void AccumulateBounds(SceneShadowBounds& aggregate, const WorldBounds& bounds)
{
    if (bounds.radius <= 0.0f) return;
    if (!aggregate.valid) {
        aggregate.center = bounds.center;
        aggregate.radius = bounds.radius;
        aggregate.valid = true;
        return;
    }

    const math::Vector3 delta = bounds.center - aggregate.center;
    const float distance = delta.Length();
    if (distance + bounds.radius <= aggregate.radius) return;
    if (distance + aggregate.radius <= bounds.radius) {
        aggregate.center = bounds.center;
        aggregate.radius = bounds.radius;
        return;
    }

    const float newRadius = (aggregate.radius + distance + bounds.radius) * 0.5f;
    if (distance > 0.0001f) {
        aggregate.center = aggregate.center + delta * ((newRadius - aggregate.radius) / distance);
    }
    aggregate.radius = newRadius;
}

bool ComputeTerrainWorldBounds(const Transform& transform, const TerrainComponent& terrain, WorldBounds& outBounds)
{
    if (!terrain.enabled || terrain.heightData.empty() || terrain.columns < 2 || terrain.rows < 2)
        return false;

    auto [minIt, maxIt] = std::minmax_element(terrain.heightData.begin(), terrain.heightData.end());
    const float minY = (*minIt) * terrain.maxHeight;
    const float maxY = (*maxIt) * terrain.maxHeight;
    const math::Vector3 localCenter = {
        static_cast<float>(terrain.columns - 1) * terrain.cellSize * 0.5f,
        (minY + maxY) * 0.5f,
        static_cast<float>(terrain.rows - 1) * terrain.cellSize * 0.5f
    };
    const math::Vector3 localExtents = {
        static_cast<float>(terrain.columns - 1) * terrain.cellSize * 0.5f,
        (maxY - minY) * 0.5f,
        static_cast<float>(terrain.rows - 1) * terrain.cellSize * 0.5f
    };

    const math::Vector4 worldCenter = transform.GetWorldMatrix()
        * math::Vector4{ localCenter.x, localCenter.y, localCenter.z, 1.0f };
    const float maxScale = (std::max)(
        (std::max)(std::abs(transform.worldScale.x), std::abs(transform.worldScale.y)),
        std::abs(transform.worldScale.z));

    outBounds.center = { worldCenter.x, worldCenter.y, worldCenter.z };
    outBounds.radius = localExtents.Length() * (std::max)(maxScale, 0.0001f);
    return outBounds.radius > 0.0f;
}

SceneShadowBounds ComputeSceneShadowBounds(Scene& scene, fbzz::LayerMask cullingMask)
{
    SceneShadowBounds result{};
    for (auto& go : scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, cullingMask)) continue;

        if (auto* mr = go.GetComponent<MeshRenderer>()) {
            if (mr->enabled && mr->mesh && !mr->mesh->isSkinned && mr->mesh->boundsRadius > 0.0f)
                AccumulateBounds(result, ComputeWorldBounds(go.transform, *mr->mesh));
        }

        if (auto* smr = go.GetComponent<SkinnedMeshRenderer>()) {
            if (smr->enabled && smr->model) {
                WorldBounds bounds{};
                if (ComputeSkinnedWorldBounds(go.transform, *smr, bounds))
                    AccumulateBounds(result, bounds);
            }
        }

        if (auto* terrain = go.GetComponent<TerrainComponent>()) {
            WorldBounds bounds{};
            if (ComputeTerrainWorldBounds(go.transform, *terrain, bounds))
                AccumulateBounds(result, bounds);
        }
    }
    return result;
}

} // namespace

void RenderSystem(Scene& scene,
                  renderer::IRenderer& renderer,
                  renderer::ResourceManager& resources,
                  const renderer::Camera& camera,
                  renderer::ResourceHandle<renderer::RenderTargetTag> outputRT,
                  const renderer::RenderSettings* settings,
                  fbzz::LayerMask cullingMask,
                  const RenderSystemUIOptions* uiOptions,
                  const physics::World* physicsWorld)
{
    FBZZ_PROFILE_SCOPE("RenderSystem");

    static renderer::RenderSettings sDefaultSettings;
    renderer::RenderSettings effectiveSettings = settings ? *settings : sDefaultSettings;
    if (const auto* runtimePostProcess = scene.TryGetRuntimePostProcessSettings())
        effectiveSettings.postProcess = *runtimePostProcess;
    const renderer::RenderSettings& rs = effectiveSettings;

    // 静的ハンドルの検証・デバイスリセット復旧・初回バッファ生成をまとめて計測する。
    profiler::Profiler::BeginSample(
        profiler::ProfilerMarker("RenderSystem::StaticResourceSetup", "Rendering"));

    // =========================================================================
    // 静的リソースの遅延初期化
    // =========================================================================
    static uint64_t sResourceResetVersion = resources.GetResetVersion();
    static uint32_t sShadowMapResolution  = 0u;
    static renderer::ResourceHandle<renderer::RenderTargetTag> shadowMapRT;
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

    static auto particleShader      = resources.LoadShader("Assets/Shaders/Material/Effects/Particle.hlsl");
    static auto particleGpuSimCS   = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuSim.cs.hlsl");
    static auto particleGpuShader  = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGPU.hlsl");
    static auto trailShader    = resources.LoadShader("Assets/Shaders/Material/Effects/Trail.hlsl");
    static auto meshTrailShader = resources.LoadShader("Assets/Shaders/Material/Effects/MeshTrail.hlsl");
    static auto skinnedMeshTrailShader = resources.LoadShader("Assets/Shaders/Material/Effects/SkinnedMeshTrail.hlsl");
    static auto detailMeshShader      = resources.LoadShader("Assets/Shaders/Detail/Detail.hlsl");
    static auto detailBillboardShader = resources.LoadShader("Assets/Shaders/Detail/Detail.hlsl"); // 同一ソース、isBillboard フラグで切り替え
    static auto detailGrassShader     = resources.LoadShader("Assets/Shaders/Detail/DetailGrass.hlsl");
    static auto detailGrassCB         = resources.CreateConstantBuffer(sizeof(DetailGrassCB));
    static auto foliageShader         = resources.LoadShader("Assets/Shaders/Foliage/Foliage.hlsl");
    static auto detailMeshPSO   = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_ON
    });
    static auto detailNoCullPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_ON
    });
    static auto foliagePSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_ON
    });
    static auto foliageNoCullPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_ON
    });

    static auto frameCB    = resources.CreateConstantBuffer(sizeof(PerFrameCB));
    static auto objectCB   = resources.CreateConstantBuffer(sizeof(PerObjectCB));
    static auto lightCB    = resources.CreateConstantBuffer(sizeof(renderer::LightConstantsCB));
    static auto shadowCB   = resources.CreateConstantBuffer(sizeof(ShadowConstantsCB));
    static auto postprocCB = resources.CreateConstantBuffer(sizeof(PostProcCB));
    static auto outlineCB  = resources.CreateConstantBuffer(sizeof(OutlineCB));
    static auto atmCB      = resources.CreateConstantBuffer(sizeof(AtmosphereCB));
    static auto decalCB    = resources.CreateConstantBuffer(sizeof(DecalCB));

    // WHY: static handle は通常フレームでは再利用し、ResourceManager::Reset() 後だけ世代差分で再生成する。
    //      これによりデバイスロスト復帰時も旧ネイティブリソースへ触らない。
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
    static auto particleAlphaPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_READ
    });
    static auto particleGpuPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ADDITIVE,
        renderer::DepthMode::DEPTH_READ
    });
    static auto particleGpuAlphaPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ALPHA_BLEND,
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

    const uint32_t shadowRes = rs.shadow.mapResolution;
    if (sResourceResetVersion != resources.GetResetVersion() || sShadowMapResolution != shadowRes || !shadowMapRT.IsValid()) {
        sResourceResetVersion = resources.GetResetVersion();
        sShadowMapResolution  = shadowRes;

        shadowMapRT = resources.CreateRenderTarget(shadowRes, shadowRes, 0);
        shadowShader        = resources.LoadShader("Assets/Shaders/Pipeline/Shadow/ShadowMap.hlsl");
        skinnedShadowShader = resources.LoadShader("Assets/Shaders/Pipeline/Shadow/SkinnedShadowMap.hlsl");
        compositeShader     = resources.LoadShader("Assets/Shaders/PostProcess/Color/Composite.hlsl");
        causticsShader      = resources.LoadShader("Assets/Shaders/PostProcess/Water/Caustics.hlsl");
        ssaoShader          = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/SSAO.cs.hlsl");
        ssaoBlurShader      = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/SSAOBlur.cs.hlsl");
        bloomDownShader     = resources.LoadShader("Assets/Shaders/PostProcess/Bloom/BloomDownsample.cs.hlsl");
        bloomUpShader       = resources.LoadShader("Assets/Shaders/PostProcess/Bloom/BloomUpsample.cs.hlsl");
        selectionMaskShader = resources.LoadShader("Assets/Shaders/Debug/SelectionMask.hlsl");
        selectionMaskSkinnedShader = resources.LoadShader("Assets/Shaders/Debug/SelectionMaskSkinnedMesh.hlsl");
        selectionOutlineShader = resources.LoadShader("Assets/Shaders/PostProcess/Outline/SelectionOutline.hlsl");
        fxaaShader = resources.LoadShader("Assets/Shaders/PostProcess/AntiAliasing/FXAA.hlsl");
        skydomeShader = resources.LoadShader("Assets/Shaders/Material/Sky/Skydome.hlsl");
        skydomeMesh   = renderer::PrimitiveMesh::Sphere(resources, 32);
        gbufferShader = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/GBuffer.hlsl");
        deferredLightingShader = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DeferredLighting.hlsl");
        depthCopyShader = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DepthCopy.hlsl");
        decalShader = resources.LoadShader("Assets/Shaders/Material/Decal/Decal.hlsl");
        decalMaskShader = resources.LoadShader("Assets/Shaders/Material/Decal/DecalMask.hlsl");
        particleShader     = resources.LoadShader("Assets/Shaders/Material/Effects/Particle.hlsl");
        particleGpuSimCS  = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuSim.cs.hlsl");
        particleGpuShader = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGPU.hlsl");
        trailShader = resources.LoadShader("Assets/Shaders/Material/Effects/Trail.hlsl");
        meshTrailShader = resources.LoadShader("Assets/Shaders/Material/Effects/MeshTrail.hlsl");
        skinnedMeshTrailShader = resources.LoadShader("Assets/Shaders/Material/Effects/SkinnedMeshTrail.hlsl");
        detailMeshShader      = resources.LoadShader("Assets/Shaders/Detail/Detail.hlsl");
        detailBillboardShader = resources.LoadShader("Assets/Shaders/Detail/Detail.hlsl");
        detailGrassShader     = resources.LoadShader("Assets/Shaders/Detail/DetailGrass.hlsl");
        detailGrassCB         = resources.CreateConstantBuffer(sizeof(DetailGrassCB));
        foliageShader         = resources.LoadShader("Assets/Shaders/Foliage/Foliage.hlsl");
        detailMeshPSO   = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID,        renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON });
        detailNoCullPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON });
        foliagePSO       = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID,        renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON });
        foliageNoCullPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON });

        bindPoseSkinningCB = {};
        {
            struct BindPoseData { math::Matrix4 bones[asset::MAX_SKINNING_BONES]; };
            BindPoseData bp{};
            for (auto& m : bp.bones) m = math::Matrix4::Identity();
            bindPoseSkinningCB = resources.CreateConstantBuffer(sizeof(BindPoseData));
            resources.Update(bindPoseSkinningCB, &bp, sizeof(BindPoseData));
        }
        frameCB    = resources.CreateConstantBuffer(sizeof(PerFrameCB));
        objectCB   = resources.CreateConstantBuffer(sizeof(PerObjectCB));
        lightCB    = resources.CreateConstantBuffer(sizeof(renderer::LightConstantsCB));
        shadowCB   = resources.CreateConstantBuffer(sizeof(ShadowConstantsCB));
        postprocCB = resources.CreateConstantBuffer(sizeof(PostProcCB));
        outlineCB  = resources.CreateConstantBuffer(sizeof(OutlineCB));
        atmCB      = resources.CreateConstantBuffer(sizeof(AtmosphereCB));
        decalCB    = resources.CreateConstantBuffer(sizeof(DecalCB));

        defaultPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON });
        wireframePSO = resources.CreatePipelineState({ renderer::RasterizerMode::WIREFRAME, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON });
        selectionMaskPso = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON });
        skydomePSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_SKY });
        particlePSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::ADDITIVE, renderer::DepthMode::DEPTH_READ });
        particleAlphaPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::ALPHA_BLEND, renderer::DepthMode::DEPTH_READ });
        particleGpuPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::ADDITIVE, renderer::DepthMode::DEPTH_READ });
        particleGpuAlphaPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::ALPHA_BLEND, renderer::DepthMode::DEPTH_READ });
        trailPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::ALPHA_BLEND, renderer::DepthMode::DEPTH_READ });
        meshTrailPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::ALPHA_BLEND, renderer::DepthMode::DEPTH_READ });
        meshTrailDoubleSidedPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::ALPHA_BLEND, renderer::DepthMode::DEPTH_READ });
        postprocPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF });
        causticsPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::ADDITIVE, renderer::DepthMode::DEPTH_OFF });
        decalPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::ALPHA_BLEND, renderer::DepthMode::DEPTH_OFF });
        decalMaskPso = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF });
    }

    // パーティクルバッファ (最大描画数分を事前確保)
    static renderer::ResourceHandle<renderer::BufferTag> particleVB;
    static renderer::ResourceHandle<renderer::BufferTag> particleIB;
    static uint64_t sParticleResetVersion = 0;
    if (sParticleResetVersion != resources.GetResetVersion()) {
        particleVB = {};
        particleIB = {};
        sParticleResetVersion = resources.GetResetVersion();
    }
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

    profiler::Profiler::EndSample();

    // UI の描画先種別を安定キーにして、Scene / Game の中間リソースを分離する。
    // uiOptions がない通常ゲーム描画は key=0 の単一コンテキストを使う。
    const uint32_t viewKey = uiOptions
        ? static_cast<uint32_t>(uiOptions->targetView) + 1u
        : 0u;
    static std::unordered_map<uint32_t, ViewRenderTargets> s_viewTargets;
    static uint64_t sRenderTargetResetVersion = 0;
    if (sRenderTargetResetVersion != resources.GetResetVersion()) {
        // ResourceManager::Reset() 後は旧ハンドルが無効なので Release せずキャッシュだけ破棄する。
        s_viewTargets.clear();
        sRenderTargetResetVersion = resources.GetResetVersion();
    }

    ViewRenderTargets& viewTargets = s_viewTargets[viewKey];
    auto& hdrRT                   = viewTargets.hdr;
    auto& ldrRT                   = viewTargets.ldr;
    auto& selectionMaskRT         = viewTargets.selectionMask;
    auto& outlineRT               = viewTargets.outline;
    auto& customPostProcessRT     = viewTargets.customPostProcess;
    auto& gbufferRT               = viewTargets.gbuffer;
    auto& decalDepthRT            = viewTargets.decalDepth;
    auto& decalMaskRT             = viewTargets.decalMask;
    auto& bloomHalf               = viewTargets.bloomHalf;
    auto& bloomFull               = viewTargets.bloomFull;
    auto& ssaoRaw                 = viewTargets.ssaoRaw;
    auto& ssaoBlur                = viewTargets.ssaoBlur;
    uint32_t& sHdrW               = viewTargets.width;
    uint32_t& sHdrH               = viewTargets.height;

    {
        FBZZ_PROFILE_SCOPE("RenderSystem::ResizeRenderTargets");
        const auto* output = resources.Get(outputRT);
        uint32_t curW = output ? output->GetWidth()  : renderer.GetWidth();
        uint32_t curH = output ? output->GetHeight() : renderer.GetHeight();
        if (curW == 0 || curH == 0) return;
        if (!hdrRT.IsValid() || sHdrW != curW || sHdrH != curH)
        {
            ReleaseViewRenderTargets(viewTargets, resources);
            hdrRT           = resources.CreateRenderTarget(curW, curH, 1);
            ldrRT           = resources.CreateRenderTarget(curW, curH, 1);
            selectionMaskRT = resources.CreateRenderTarget(curW, curH, 1);
            outlineRT       = resources.CreateRenderTarget(curW, curH, 1);
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
    // ライト収集とシャドウ範囲計算はシーン全体を走査するため、独立して計測する。
    profiler::Profiler::BeginSample(
        profiler::ProfilerMarker("RenderSystem::LightingSetup", "Rendering"));

    // ライト定数バッファを構築
    // =========================================================================
    renderer::LightConstantsCB lightData{};
    lightData.lightDir       = { 0.0f, -1.0f, 0.5f };
    lightData.lightColor     = { 1.0f,  1.0f, 1.0f };
    lightData.lightIntensity = 1.0f;

    // Directional Light のシャドウ設定 (LightComponent から取得)
    bool  dirCastShadows    = true;
    float dirShadowBias     = 1.0f;
    float dirShadowStrength = 1.0f;
    float dirShadowDistance = 0.0f;

    constexpr float kDegToRad = 3.14159265f / 180.0f;
    for (auto [tf, lc] : scene.View<Transform, LightComponent>()) {
        if (!lc.enabled) continue;
        if (lc.type == LightComponent::Type::Directional) {
            lightData.lightDir       = tf.forward.Normalized();
            lightData.lightColor     = lc.color;
            lightData.lightIntensity = lc.intensity;
            dirCastShadows    = lc.castShadows;
            dirShadowBias     = lc.shadowBias;
            dirShadowStrength = lc.shadowStrength;
            dirShadowDistance = lc.shadowDistance;
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
            sl.direction = tf.forward.Normalized();
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

    math::Vector3 lightDir = lightData.lightDir.Normalized();
    SceneShadowBounds shadowBounds;
    {
        FBZZ_PROFILE_SCOPE("RenderSystem::ComputeShadowBounds");
        shadowBounds = ComputeSceneShadowBounds(scene, cullingMask);
    }
    if (!shadowBounds.valid) {
        shadowBounds.center = camera.m_position + camera.GetForward() * 20.0f;
        shadowBounds.radius = 40.0f;
        shadowBounds.valid = true;
    }

    // dirShadowDistance > 0 のとき手動サイズを優先、0 のときシーンに自動フィット
    const float shadowRadius = (dirShadowDistance > 0.0f)
        ? dirShadowDistance
        : (std::max)(shadowBounds.radius, 5.0f);
    math::Vector3 lightPos = shadowBounds.center - lightDir * (shadowRadius + 20.0f);
    math::Vector3 up = (std::abs(lightDir.y) > 0.99f)
                       ? math::Vector3{ 1.0f, 0.0f, 0.0f }
                       : math::Vector3{ 0.0f, 1.0f, 0.0f };
    math::Matrix4 lightView = math::Matrix4::LookAt(lightPos, shadowBounds.center, up);
    math::Matrix4 lightProj = math::Matrix4::Orthographic(-shadowRadius, shadowRadius,
                                                           -shadowRadius, shadowRadius,
                                                           1.0f,
                                                           shadowRadius * 2.0f + 40.0f);
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
    profiler::Profiler::EndSample();

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
    passHandles.particleAlphaPSO     = particleAlphaPSO;
    passHandles.particleVB           = particleVB;
    passHandles.particleIB           = particleIB;
    passHandles.particleGpuSimCS     = particleGpuSimCS;
    passHandles.particleGpuShader    = particleGpuShader;
    passHandles.particleGpuAlphaShader = particleGpuShader; // 同一シェーダー、PSO で合成モードを切り替える
    passHandles.particleGpuPSO       = particleGpuPSO;
    passHandles.particleGpuAlphaPSO  = particleGpuAlphaPSO;
    passHandles.trailShader          = trailShader;
    passHandles.trailPSO             = trailPSO;
    passHandles.meshTrailShader      = meshTrailShader;
    passHandles.skinnedMeshTrailShader = skinnedMeshTrailShader;
    passHandles.meshTrailPSO         = meshTrailPSO;
    passHandles.meshTrailDoubleSidedPSO = meshTrailDoubleSidedPSO;
    passHandles.detailMeshShader      = detailMeshShader;
    passHandles.detailBillboardShader = detailBillboardShader;
    passHandles.detailGrassShader     = detailGrassShader;
    passHandles.detailGrassCB         = detailGrassCB;
    passHandles.detailMeshPSO         = detailMeshPSO;
    passHandles.detailNoCullPSO       = detailNoCullPSO;
    passHandles.foliageShader          = foliageShader;
    passHandles.foliagePSO             = foliagePSO;
    passHandles.foliageNoCullPSO       = foliageNoCullPSO;
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
        lightData, lightVP, isDeferred, ssaoEnabled, 0.0f, 1.0f,
        &cameraFrustum, &lightFrustum, &occlusionCuller
    };
    passCtx.physicsWorld  = physicsWorld;
    // near=1.0, far=shadowRadius*2+40 の深度範囲でスケール正規化したバイアス。
    // 固定 NDC 値はシーンが広がるほど Peter Panning が悪化するため、
    // ワールド空間で約 5mm 相当の一定バイアスになるよう depthRange で除算する。
    // dirShadowBias でスケールし、Inspector から Peter Panning / アクネをチューニング可能にする。
    passCtx.shadowBiasNDC  = (0.005f * dirShadowBias) / (shadowRadius * 2.0f + 39.0f);
    passCtx.shadowStrength = dirCastShadows ? dirShadowStrength : 0.0f;

    // =========================================================================
    // RenderPipeline にパスを登録
    // =========================================================================
    RenderPipeline pipeline;
    profiler::Profiler::BeginSample(
        profiler::ProfilerMarker("RenderSystem::BuildPipeline", "Rendering"));
    pipeline.DeclareResource("Output",     { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, true,  false });
    pipeline.DeclareResource("ShadowMap",  { renderer::RenderGraph::ResourceKind::RenderTarget, rs.shadow.mapResolution, rs.shadow.mapResolution, 0, false, false });
    pipeline.DeclareResource("HDR",        { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    pipeline.DeclareResource("LDR",        { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    pipeline.DeclareResource("SelectionMask", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    pipeline.DeclareResource("Outline",    { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    pipeline.DeclareResource("SceneColor", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    pipeline.DeclareResource("CustomPostProcess0", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    pipeline.DeclareResource("CustomPostProcess1", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    pipeline.DeclareResource("Bloom",      { renderer::RenderGraph::ResourceKind::Texture, sHdrW, sHdrH, 0, false, true });
    if (isDeferred)
        pipeline.DeclareResource("GBuffer", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, false });
    if (ssaoEnabled)
        pipeline.DeclareResource("SSAO", { renderer::RenderGraph::ResourceKind::Texture, sHdrW, sHdrH, 0, false, true });
    pipeline.SetOutputs({ "Output" });

    scene.ClearUserRenderPasses();

    auto appendQueuedUserPasses = [&](UserRenderPassInjectionPoint injectionPoint) {
        for (const auto& desc : scene.GetUserRenderPasses()) {
            if (desc.injectionPoint != injectionPoint)
                continue;
            assert(!desc.name.empty() && "UserRenderPassDesc.name is required");
            auto execute = desc.execute;
            pipeline.AddRawPass(desc.name, desc.accesses, [&, execute]() {
                if (execute)
                    execute(passCtx);
            }, desc.allowCulling);
        }
    };

    // ── Shadow ────────────────────────────────────────────────────────────────
    pipeline.AddRawPass("Shadow", {}, { "ShadowMap" }, [&]() {
        ExecuteShadowPass(passCtx);
    });

    // ── Forward or Deferred ───────────────────────────────────────────────────
    if (!isDeferred) {
        pipeline.AddRawPass("ForwardOpaque", { "ShadowMap" }, { "HDR" }, [&]() {
            ExecuteForwardPasses(passCtx);
        });
    }

    if (isDeferred) {
        pipeline.AddRawPass("DeferredGBuffer", { "ShadowMap" }, { "GBuffer" }, [&]() {
            ExecuteGBufferPass(passCtx);
        });

        pipeline.AddRawPass("DeferredDepthCopy", { "GBuffer" }, { "HDR" }, [&]() {
            ExecuteDeferredDepthCopyPass(passCtx);
        });
    }

    // ── Terrain / Detail / Foliage ────────────────────────────────────────────
    // ForwardOpaque / GBuffer DepthCopy の後・Sky の前に描画する。
    // WHY: Sky より前に描画することで地形の上に空が被らない。
    //      ForwardOpaque と同じ HDR RT (depth buffer 共有) で描画することで
    //      Player 等の不透明オブジェクトと正しく depth test される。
    //      RenderSystem 内に統合することでポストプロセス（bloom/SSAO等）も適用される。
    pipeline.AddPass<TerrainRenderPass>();
    pipeline.AddPass<DetailRenderPass>();
    pipeline.AddPass<FoliageRenderPass>();

    pipeline.AddRawPass("Sky", { "HDR" }, { "HDR" }, [&]() {
        ExecuteSkyPass(passCtx);
    });

    // ── SSAO + Deferred Lighting ──────────────────────────────────────────────
    if (isDeferred) {
        if (ssaoEnabled) {
            pipeline.AddRawPass("SSAO", { "GBuffer" }, { "SSAO" }, [&]() {
                ExecuteSSAOPass(passCtx);
            });
            pipeline.AddRawPass("DeferredLighting", { "GBuffer", "HDR", "SSAO" }, { "HDR" }, [&]() {
                ExecuteDeferredLightingPass(passCtx);
            });
        } else {
            pipeline.AddRawPass("DeferredLighting", { "GBuffer", "HDR" }, { "HDR" }, [&]() {
                ExecuteDeferredLightingPass(passCtx);
            });
        }

        pipeline.AddRawPass("DeferredSkinnedForward", { "HDR" }, { "HDR" }, [&]() {
            ExecuteDeferredSkinnedForwardPass(passCtx);
        });

        pipeline.AddRawPass("DeferredForwardTransparent", { "HDR" }, { "HDR" }, [&]() {
            ExecuteDeferredForwardTransparentPass(passCtx);
        });
    }

    pipeline.AddPass<WaterRenderPass>();

    for (EntityID id : scene.GetEntities<ScriptComponent>()) {
        auto* sc = scene.GetComponent<ScriptComponent>(id);
        auto* go = scene.GetGameObject(id);
        if (!sc || !go)
            continue;
        for (auto& entry : sc->scripts) {
            if (!entry.script || !entry.script->enabled)
                continue;
            entry.script->SetContext(&scene, go);
            entry.script->OnSetupRenderPasses(pipeline, passCtx);
        }
    }

    appendQueuedUserPasses(UserRenderPassInjectionPoint::AfterOpaque);

    // ── デカール用深度スナップショット ────────────────────────────────────────
    pipeline.DeclareResource("DecalDepth", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    pipeline.AddRawPass("DecalDepthCopy", { isDeferred ? "GBuffer" : "HDR" }, { "DecalDepth" }, [&]() {
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
    pipeline.AddRawPass("Decal", { "HDR", "DecalDepth" }, { "HDR" }, [&]() {
        ExecuteDecalPass(passCtx);
    });

    pipeline.AddPass<MeshTrailRenderPass>();
    pipeline.AddPass<TrailRenderPass>();

    pipeline.AddRawPass("Particle", { "HDR" }, { "HDR" }, [&]() {
        ExecuteParticlePass(passCtx);
    });

    appendQueuedUserPasses(UserRenderPassInjectionPoint::AfterTransparent);

    // ── Selection / Debug ─────────────────────────────────────────────────────
    pipeline.AddRawPass("UnderwaterCaustics", { "HDR" }, { "HDR" }, [&]() {
        ExecuteCausticsPass(passCtx);
    });

    if (selectionOutlineEnabled) {
        pipeline.AddRawPass("SelectionMask", { "HDR" }, { "SelectionMask" }, [&]() {
            ExecuteSelectionMaskPass(passCtx);
        });
    }

    pipeline.AddPass<DebugCollidersPass>();
    pipeline.AddPass<ConstraintDebugPass>();
    pipeline.AddPass<AnimatorDebugPass>();
    pipeline.AddPass<GridDebugPass>();
    pipeline.AddPass<LightRangeDebugPass>();
    pipeline.AddPass<TerrainCollisionDebugPass>();

    pipeline.AddRawPass("ScriptDebugDraw", { "HDR" }, { "HDR" }, [&]() {
        scene.TickScriptDebugDrawCommands(Time::deltaTime);
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

    pipeline.AddPass<NavMeshDebugPass>();
    pipeline.AddPass<DecalDebugPass>();

    appendQueuedUserPasses(UserRenderPassInjectionPoint::BeforePostProcess);

    // ── PostProcess チェーン ──────────────────────────────────────────────────
    if (rs.postProcess.bloom.enabled) {
        pipeline.AddRawPass("Bloom", { "HDR" }, { "Bloom" }, [&]() {
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
        pipeline.AddRawPass("Composite", { "HDR", "Bloom" }, { needsLdrIntermediate ? "LDR" : "Output" }, [&]() {
            ExecuteCompositePass(passCtx);
        });
    } else {
        pipeline.AddRawPass("Composite", { "HDR" }, { needsLdrIntermediate ? "LDR" : "Output" }, [&]() {
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

            pipeline.AddRawPass(
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
        pipeline.AddRawPass("SelectionOutline",
            { std::string_view(postCustomResource), "SelectionMask" },
            { rs.postProcess.fxaaEnabled ? "Outline" : "Output" },
            [&]() { ExecuteSelectionOutlinePass(passCtx); });
    }

    if (rs.postProcess.fxaaEnabled) {
        if (selectionOutlineEnabled) {
            pipeline.AddRawPass("FXAA", { "Outline" }, { "Output" }, [&]() { ExecuteFxaaPass(passCtx); });
        } else if (customPostProcessEnabled) {
            pipeline.AddRawPass("FXAA", { std::string_view(postCustomResource) }, { "Output" }, [&]() { ExecuteFxaaPass(passCtx); });
        } else {
            pipeline.AddRawPass("FXAA", { "LDR" }, { "Output" }, [&]() { ExecuteFxaaPass(passCtx); });
        }
    }

    if (uiOptions && uiOptions->enabled && uiOptions->context) {
        pipeline.AddRawPass(
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
                         *uiOptions->context,
                         uiWidth,
                         uiHeight,
                         uiOptions->mouseInCanvasSpace,
                         uiOptions->mousePressed,
                         camera.m_position,
                         camera.m_rotation,
                         camera.GetViewProjection(),
                         uiOptions->targetView);
            });
    }
    profiler::Profiler::EndSample();

    {
        FBZZ_PROFILE_SCOPE("RenderSystem::ScriptPreRender");
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
    }

    // =========================================================================
    // RenderPipeline 実行 + デバッグスナップショット更新
    // =========================================================================

    // GPU Timestamp Query の前フレーム結果を収集してからフレームを開始する。
    // WHY: GpuProfCollect を先に呼ぶことで前フレームの非同期クエリが確定している可能性を最大化する。
    {
        FBZZ_PROFILE_SCOPE("RenderSystem::GpuProfilerSetup");
        renderer.GpuProfCollect();
        renderer.GpuProfBeginFrame();

        // GPU フックを RenderPipeline に設定する。CPU フックとは独立しているため、
        // Profiler の CPU スコープ計測と干渉しない。
        pipeline.SetGpuProfilerHooks(
            [&](std::string_view name) { renderer.GpuProfBeginPass(name.data()); },
            [&](std::string_view name) { renderer.GpuProfEndPass(name.data()); }
        );
    }

    const bool graphExecuted = pipeline.Execute(passCtx);

    renderer.GpuProfEndFrame();
    assert(graphExecuted);
    (void)graphExecuted;

    {
        FBZZ_PROFILE_SCOPE("RenderSystem::ScriptPostRender");
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
    }

    {
        FBZZ_PROFILE_SCOPE("RenderSystem::DebugSnapshot");
        renderer::RenderDebugOverlay::Snapshot dbgSnap;
        dbgSnap.hdrRT           = hdrRT;
        dbgSnap.ldrRT           = ldrRT;
        dbgSnap.selectionMaskRT = selectionMaskRT;
        dbgSnap.outlineRT       = outlineRT;
        dbgSnap.gbufferRT       = gbufferRT;
        dbgSnap.width           = sHdrW;
        dbgSnap.height          = sHdrH;
        for (const auto& profile : pipeline.LastReport().profiles)
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
