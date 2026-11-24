/// @file    RenderSystem.cpp
/// @brief   Scene から DrawCall を生成するオーケストレーター。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#include "Engine/Rendering/RenderSystem.hpp"
#include <Engine/Asset/Streaming/StreamedTextureResolver.hpp>
#include "Engine/Scene/Hierarchy/SceneUtils.hpp"
#include <Graphics/Renderer/View/OpaqueRenderPlan.hpp>
#include <Engine/Rendering/Extraction/RenderSceneExtractor.hpp>
#include "../Scene/Systems/Water/WaterfallFluidRuntime.hpp"
#include <Engine/Rendering/Extraction/Lighting/RenderLightExtractor.hpp>
#include <Graphics/Pipeline/Geometry/GeometryPipeline.hpp>
#include "Graphics/Renderer/Diagnostics/RenderDebugOverlay.hpp"
#include <Graphics/Pipeline/Capture/RenderPassCapture.hpp>
#include "Graphics/Renderer/Diagnostics/DebugDraw.hpp"
#include "Passes/Debug/DebugPasses.hpp"
#include <Physics/World/World.hpp>
#include "Passes/Geometry/GeometryPasses.hpp"
#include "Passes/Geometry/SkinningOutputDemand.hpp"
#include <Core/Profiler/FrameTimeline.hpp>
#include "Extraction/MaterialDemandTrace.hpp"
#include "Extraction/RenderCandidateLease.hpp"
#include "Extraction/Lighting/RenderShadowPreparation.hpp"
#include "RenderViewSettings.hpp"
#include "Extraction/Lighting/RenderDirectionalShadowBake.hpp"
#include "../Scene/Systems/UI/CanvasGeometryGuard.hpp"
#include "Passes/PostProcess/PostProcessPasses.hpp"
#include <Graphics/Pipeline/Resources/RenderResources.hpp>
#include <Graphics/Pipeline/View/ViewPipeline.hpp>
#include <Graphics/Pipeline/View/ViewPreparation.hpp>
#include <Graphics/Pipeline/Geometry/InstanceBatch.hpp>
#include <Engine/Rendering/Passes/RenderPassContext.hpp>
#include "Passes/Debug/SelectionPasses.hpp"
#include "Engine/Core/Platform/Application.hpp"
#include "Engine/Core/Timing/Time.hpp"
#include <Engine/Core/Platform/DeveloperMode.hpp>
#include "Engine/Core/Diagnostics/Logger.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Scripting/ScriptComponent.hpp"
#include <Engine/Scene/Components/UI/UICanvas.hpp>
#include "Engine/Scene/Components/Lighting/ReflectionProbeComponent.hpp"
#include <Engine/Scene/Components/Lighting/LightProbeVolumeComponent.hpp>
#include "Engine/Scene/Components/Geometry/SkinnedMeshRenderer.hpp"
#include "Engine/Asset/AssetManager.hpp"
#include "Graphics/Renderer/IRenderer.hpp"
#include "Graphics/Renderer/View/Camera.hpp"
#include "Graphics/Renderer/Diagnostics/PipelineDiagnostics.hpp"
#include "Graphics/Renderer/View/Lighting/LightSystem.hpp"
#include "Engine/Rendering/Passes/RenderPipeline.hpp"
#include "Graphics/Renderer/Resources/ResourceManager.hpp"
#include <Graphics/Renderer/Diagnostics/RenderCpuTrace.hpp>
#include <Graphics/Renderer/Geometry/SkinningPolicy.hpp>
#include "Engine/Asset/Animation/Rig/Skeleton.hpp"
#include <Engine/Profiler/ProfileScope.hpp>
#include <Math/Geometry/Frustum.hpp>
#include <Math/LinearAlgebra/Matrix4.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::scene {
namespace cpu_trace = renderer::cpu_trace;

static_assert(asset::MAX_SKINNING_BONES == renderer::RENDER_SKINNING_BONES);

namespace {

/// @note 要求フィルターは実際の収録に先行し、スコープは収録中のフィルターで絞り込む。
bool CollectFrameGeometryDiagnostics()
{
    if (!profiler::Profiler::IsEnabled()) return false;
    const auto filter = profiler::Profiler::GetRequestedCaptureFilter();
    return filter.scope == "RenderSystem::FrameGeometry"
        && (filter.category.empty() || filter.category == "Rendering");
}
/// @note ScriptComponent があれば状態を問わず、描画コールバックによる変更を見込んで共有を避ける。
/// @note スクリプトがない場合も Canvas の状態を確認する。
bool CanShareFrameGeometry(Scene& scene)
{
    const bool diagnostics = CollectFrameGeometryDiagnostics();
    std::optional<profiler::ProfileScope> scriptProfile;
    if (diagnostics) scriptProfile.emplace(
        profiler::ProfilerMarker("RenderSystem::FrameGeometry::ScriptGuard", "Rendering"));
    if (!scene.GetEntities<ScriptComponent>().empty()) return false;
    scriptProfile.reset();
    std::optional<profiler::ProfileScope> canvasProfile;
    if (diagnostics) canvasProfile.emplace(
        profiler::ProfilerMarker("RenderSystem::FrameGeometry::CanvasGuard", "Rendering"));
    return canvas_geometry_detail::CanShareCanvasFrameGeometry(scene);
}

/// @note 少数の TAA サンプルを均等に分散させるため、指定した基数の桁を反転する。
float HaltonRadicalInverse(uint32_t index, uint32_t base)
{
    const float invBase = 1.0f / static_cast<float>(base);
    float result   = 0.0f;
    float fraction = invBase;
    while (index > 0u) {
        result   += static_cast<float>(index % base) * fraction;
        index    /= base;
        fraction *= invBase;
    }
    return result;
}

struct ViewShadowPreparation {
    renderer::ShadowBounds bounds;
    renderer::PreparedShadows shadows;
    uint32_t staticMask = 0;
    uint32_t reuseMask = 0;
};

ViewShadowPreparation PrepareViewShadows(Scene& scene, renderer::IRenderer& renderer,
    renderer::ResourceManager& resources, const renderer::Camera& camera, const renderer::RenderSettings& rs,
    const renderer::RenderSharedResources& shared, renderer::RenderViewResources& viewTargets,
    const RenderLightExtraction& lighting, const math::Vector3& lightDir, fbzz::LayerMask cullingMask)
{
    ViewShadowPreparation result;
    bool skinningReadAhead = false;
    if (renderer::UseSkinningWaitOptimization() && renderer::UseOwnedSkinningBatch()
        && resources.Get(shared.skinningComputeCS) && resources.Get(shared.skinningCB)) {
        const auto skinned = scene.GetEntities<SkinnedMeshRenderer>();
        const bool hasWarmSkinning = std::any_of(skinned.begin(), skinned.end(), [&](EntityID id) {
            const auto* object = scene.GetGameObject(id);
            const auto* skin = scene.GetComponent<SkinnedMeshRenderer>(id);
            return object && object->activeInHierarchy() && skin && skin->enabled && skin->lodVisible
                && skin->gpuSkinnedThisFrame && skin->model && skin->skinnedBufferModel == skin->model;
        });
        if (hasWarmSkinning) skinningReadAhead = renderer::TryStartOwnedSkinningReadAhead(renderer);
    }
    /// @note 行列は影無効でもネイティブ・材質・粒子・体積光へ公開するため、パスの設定で省略しない。
    auto& shadowBounds = result.bounds;
    shadowBounds = shadow_preparation_detail::ResolveSceneBounds(scene, cullingMask);
    if (!shadowBounds.valid) {
        shadowBounds.center = camera.m_position + camera.GetForward() * 20.0f;
        shadowBounds.radius = 40.0f;
        shadowBounds.valid = true;
    }

    const auto cachedLight = lighting.dirCachedFarShadowLight;
    const bool requestFarCache = cachedLight.IsValid() && rs.shadowEnabled && rs.shadow.cascadeCount > 1
        && Script::IsInPlayMode() && renderer.GetCapabilities().depthRegionClear;
    auto& shadows = result.shadows;
    shadows = renderer::PrepareShadows(camera, rs, shadowBounds, lightDir,
        lighting.dirShadowDistance, lighting.dirShadowBias, requestFarCache);
    /// @note 遅延ディスクリプターをスキニング側が参照し得るため、GPU 資源を操作する前に先行処理を完了させる。
    if (skinningReadAhead) renderer::FinishOwnedSkinningReadAhead(renderer);
    auto& shadowAtlas = viewTargets.staticShadowAtlas;
    auto& shadowCaches = viewTargets.shadowCascadeCaches;
    if (requestFarCache) {
        if (shadowAtlas.Ensure(resources, rs.shadow.mapResolution, rs.shadow.mapResolution, 0))
            shadowCaches = {};
        if (resources.Get(resources.GetDepthTexture(shadowAtlas.Handle())) && resources.Get(shared.depthCopyShader)) {
            const renderer::FarShadowCacheKey key{scene.GetRenderSceneGeneration(), scene.GetHierarchyRevision(),
                shadowBounds.terrainRevision, (static_cast<uint64_t>(cachedLight.generation) << 32u) | cachedLight.index,
                cullingMask};
            for (int index = std::min(2, shadows.count - 1); index < shadows.count; ++index) {
                result.staticMask |= 1u << index;
                if (renderer::PrepareShadowCascadeCache(shadowCaches[index], shadows, camera, lightDir, key,
                        Time::frameCount, index))
                    result.reuseMask |= 1u << index;
            }
        } else {
            shadowAtlas.Release(resources);
            shadowCaches = {};
            shadows = renderer::PrepareShadows(camera, rs, shadowBounds, lightDir,
                lighting.dirShadowDistance, lighting.dirShadowBias);
        }
    } else if (shadowAtlas.IsValid()) {
        shadowAtlas.Release(resources);
        shadowCaches = {};
    }
    return result;
}

template<typename Callback>
void ForEachActiveRenderScript(Scene& scene, Callback&& callback)
{
    for (EntityID id : scene.GetEntities<ScriptComponent>()) {
        auto* sc = scene.GetComponent<ScriptComponent>(id);
        auto* go = scene.GetGameObject(id);
        if (!sc || !go || !go->activeInHierarchy())
            continue;
        for (auto& entry : sc->scripts) {
            if (!entry.script || !entry.script->enabled)
                continue;
            entry.script->SetContext(&scene, go);
            callback(*entry.script);
        }
    }
}

void PrepareViewEnvironment(RenderPassContext& passCtx, renderer::RenderViewResources& viewTargets,
    IblSource activeIblSource, float screenAoStrength, float screenContactShadowStrength)
{
    auto& scene = passCtx.scene;
    auto& resources = passCtx.resources;
    auto& passHandles = passCtx.handles;
    const auto& rs = passCtx.settings;
    auto& environmentResources = *passCtx.environmentResources;
    /// @note グラフの外で空を捕捉し、同じビューの照明が使う IBL を用意する。
    const auto environmentProfile = profiler::Profiler::BeginScope(
        profiler::ProfilerMarker("RenderSystem::EnvironmentSetup", "Rendering"));
    const auto environmentTimeline = profiler::frame_timeline::Begin(profiler::frame_timeline::Event::ENVIRONMENT);
    bool dynamicIblReady = false;
    bool reflectionProbeSelected = false;
    float reflectionProbeIntensity = 1.0f;
    int dynamicIblMipCount = 0;
    /// @note Unlit の消灯済み skyDimmer を共有キューブへ焼くと、別の Lit ビューも黒い照明を読む。
    /// @see Assets/Shaders/Material/Sky/Skydome.hlsl PSMain の skyDimmer。
    if (!rs.IsUnlit() && activeIblSource == IblSource::DynamicSky) {
        ExecuteSkyCapturePass(passCtx, Script::IsInPlayMode());
        ExecuteSkyLightBakePass(passCtx);
        if (environmentResources.HasBakedTextures()) {
            passHandles.iblIrradiance = environmentResources.skyIrradiance;
            passHandles.iblPrefilter  = environmentResources.skyPrefilter;
            dynamicIblReady = true;
            dynamicIblMipCount = static_cast<int>(environmentResources.prefilteredMipCount);
            passCtx.iblIrradiancePublication = {environmentResources.immutableIrradiance, environmentResources.immutableIblOwner,
                environmentResources.immutableIblEpoch};
            passCtx.iblPrefilterPublication = {environmentResources.immutablePrefilter, environmentResources.immutableIblOwner,
                environmentResources.immutableIblEpoch};
        }
    }

    /// @note 局所 Reflection Probe はカメラが影響範囲内にいるとき、グローバル IBL より優先する。
    const auto reflectionProbeProfile = profiler::Profiler::BeginScope(
        profiler::ProfilerMarker("RenderSystem::ReflectionProbeCapture", "Rendering"));
    material_demand_detail::CompleteBindings(passCtx);
    if (auto* localProbe = rs.IsUnlit() ? nullptr : ExecuteReflectionProbeCapturePass(passCtx)) {
        reflectionProbeSelected = true;
        passHandles.iblIrradiance = localProbe->runtimeIrradiance;
        passHandles.iblPrefilter  = localProbe->runtimePrefilter;
        dynamicIblReady = true;
        reflectionProbeIntensity = localProbe->intensity;
        dynamicIblMipCount = static_cast<int>(localProbe->runtimePrefilterMipCount);
        passCtx.iblIrradiancePublication = {};
        passCtx.iblPrefilterPublication = {};
        if (localProbe->runtimeImmutablePublished) {
            passCtx.iblIrradiancePublication = {localProbe->runtimePublishedIrradiance, localProbe->runtimePublicationOwner,
                localProbe->runtimePublicationEpoch};
            passCtx.iblPrefilterPublication = {localProbe->runtimePublishedPrefilter, localProbe->runtimePublicationOwner,
                localProbe->runtimePublicationEpoch};
        }
    }

    profiler::Profiler::EndScope(reflectionProbeProfile);

    const auto advancedConstantsProfile = profiler::Profiler::BeginScope(
        profiler::ProfilerMarker("RenderSystem::AdvancedConstants", "Rendering"));
    const ActiveWeather weather = FindActiveWeather(scene);
    renderer::PrepareAdvancedConstants(passCtx, viewTargets,
        { dynamicIblReady, reflectionProbeIntensity, dynamicIblMipCount,
          screenAoStrength, screenContactShadowStrength,
          weather.wetness, weather.darkening, weather.puddleAmount, reflectionProbeSelected },
        [&](AdvancedGraphicsCB& agData) {
            FBZZ_PROFILE_SCOPE("RenderSystem::LightProbeBake");
            material_demand_detail::CompleteBindings(passCtx);
            const LightProbeVolumeSelection gi = ExecuteLightProbeBakePass(passCtx, agData);
            const LightProbeVolumeSelection::Entry* slots[2] = { &gi.inner, &gi.outer };
            for (int slot = 0; slot < 2; ++slot) {
                if (!slots[slot]->volume) continue;
                FillLightProbeVolumeConstants(*slots[slot]->owner, *slots[slot]->volume, agData.probeVolumes[slot]);
                passHandles.lightProbeSH[slot] = slots[slot]->volume->runtimeVolume;
                passHandles.lightProbeVisibility[slot] = slots[slot]->volume->runtimeVisibility;
                if (const auto* visibility = resources.Get(passHandles.lightProbeVisibility[slot]))
                    agData.probeVisibilityIndices[slot] = visibility->GetBindlessIndex();
            }
            /// @note 鏡面遮蔽の強度はビューにつき 1 つなので、内側のライトプローブを採用する。
            if (gi.inner.volume)
                agData.probeSpecularOcclusion = std::clamp(gi.inner.volume->specularOcclusion, 0.0f, 1.0f);
        });
    profiler::Profiler::EndScope(advancedConstantsProfile);
    profiler::Profiler::EndScope(environmentProfile);
    profiler::frame_timeline::End(environmentTimeline);
}

/// @pre パイプラインの実行が終わるまで passCtx と scriptGizmoCapture は有効であること。
renderer::ViewPipelineExtensions MakeViewPipelineExtensions(
    RenderPipeline& pipeline, RenderPassContext& passCtx, renderer::DebugDrawCapture& scriptGizmoCapture,
    const RenderSystemUIOptions* uiOptions)
{
    auto& scene = passCtx.scene;
    renderer::ViewPipelineExtensions extensions;
    extensions.selectionMask = [&]() { pipeline.AddPass<SelectionMaskPass>(); };
    extensions.selectionOutline = [&](renderer::PassResources& res) { ExecuteSelectionOutlinePass(res, passCtx); };
    extensions.begin = [&]() { scene.ClearUserRenderPasses(); };
    extensions.setup = [&]() {
        material_demand_detail::CompleteBindings(passCtx);
        ForEachActiveRenderScript(scene, [&](Script& script) {
            script.ExecuteProfiledCallback(&Script::OnSetupRenderPasses, pipeline, passCtx,
                ScriptCallbackKind::SETUP_RENDER_PASSES, "OnSetupRenderPasses");
        });
    };
    extensions.userPasses = [&](UserRenderPassInjectionPoint injectionPoint) {
        for (const auto& desc : scene.GetUserRenderPasses()) {
            if (desc.injectionPoint != injectionPoint)
                continue;
            assert(!desc.name.empty() && "UserRenderPassDesc.name is required");
            auto execute = desc.execute;
            pipeline.AddRawPass(desc.name, desc.accesses, [&passCtx, execute]() {
                if (execute)
                    execute(passCtx);
            }, desc.allowCulling);
        }
    };
    extensions.depthDebug = [&]() {
        scene.TickScriptDebugDrawCommands(Time::deltaTime);
        CaptureScriptGizmos(passCtx, scriptGizmoCapture);
        pipeline.AddPass<GridDebugPass>();
        pipeline.AddPass<NavMeshDebugPass>();
        pipeline.AddPass<ScriptGizmoPass>("HDR", "ScriptGizmosDepth",
                                          renderer::DebugDrawLayer::DepthTested, scriptGizmoCapture);
    };
    extensions.overlayDebug = [&](const char* chainOutRes) {
        pipeline.AddPass<ConstraintDebugPass>(chainOutRes);
        pipeline.AddPass<RagdollDebugPass>(chainOutRes);
        pipeline.AddPass<RigidBodyDebugPass>(chainOutRes);
        pipeline.AddPass<AnimatorDebugPass>(chainOutRes);
        pipeline.AddPass<IKDebugPass>(chainOutRes);
        pipeline.AddPass<SpringBoneDebugPass>(chainOutRes);
        pipeline.AddPass<AttachmentDebugPass>(chainOutRes);
        pipeline.AddPass<LightRangeDebugPass>(chainOutRes);
        pipeline.AddPass<VFXGizmoDebugPass>(chainOutRes);
        pipeline.AddPass<FlowFieldDebugPass>(chainOutRes);
        pipeline.AddPass<FlowSampleDebugPass>(chainOutRes);
        pipeline.AddPass<PhysicsVolumeDebugPass>(chainOutRes);
        pipeline.AddPass<WaterFlowDebugPass>(chainOutRes);
        pipeline.AddPass<VFXPathDebugPass>(chainOutRes);
        pipeline.AddPass<BoundsDebugPass>(chainOutRes);
        pipeline.AddPass<TerrainCollisionDebugPass>(chainOutRes);
        pipeline.AddPass<DecalDebugPass>(chainOutRes);
        pipeline.AddPass<ScriptGizmoPass>(chainOutRes, "ScriptGizmos",
                                          renderer::DebugDrawLayer::Overlay, scriptGizmoCapture);
        pipeline.AddPass<DebugCollidersPass>(chainOutRes);
    };
    extensions.ui = [&pipeline, &passCtx, uiOptions]() {
        if (uiOptions && uiOptions->enabled && uiOptions->context) {
            pipeline.AddRawPass(
                "UIPass",
                { { "Output", renderer::RenderGraph::ResourceUsage::ReadWrite },
                  { "HDR", renderer::RenderGraph::ResourceUsage::Read } },
                [&passCtx]() { ExecuteUIPass(passCtx); });
        }
    };
    return extensions;
}

void ValidateReferencePoseBuffers(Scene& scene, renderer::ResourceManager& resources)
{
    FBZZ_PROFILE_SCOPE("RenderSystem::SkinningResourceValidation");
    for (EntityID id : scene.GetEntities<SkinnedMeshRenderer>()) {
        const auto* smr = scene.GetComponent<SkinnedMeshRenderer>(id);
        if (smr && smr->model && !resources.Get(smr->model->referencePoseCB))
            smr->model->referencePoseCB = {};
    }
}

/// @note 同じモデルの全インスタンスで 1 本のバインド姿勢パレットを共有する。
void PrepareReferencePoseBuffers(Scene& scene, renderer::ResourceManager& resources)
{
    FBZZ_PROFILE_SCOPE("RenderSystem::ReferencePoseBuffers");
    struct ReferencePoseData { math::Matrix4 bones[asset::MAX_SKINNING_BONES]; };

    /// @note シェーダー読込後に候補を取り直し、資源確保をまたいで Component 配列を借用しない。
    detail::RenderCandidateLease candidateLease;
    auto& candidates = candidateLease.Entities();
    scene.CopyRendererEntities(candidates, Scene::RendererEntitySet::SKINNED);
    const auto sceneGeneration = scene.GetRenderSceneGeneration();
    for (EntityID id : candidates) {
        if (scene.GetRenderSceneGeneration() != sceneGeneration) break;
        const auto* smr = scene.GetComponent<SkinnedMeshRenderer>(id);
        if (!smr || !smr->model || !smr->model->skeleton || smr->model->referencePoseCB.IsValid())
            continue;

        const auto* model = smr->model;
        const auto& refPose = model->skeleton->referencePose;
        if (refPose.empty()) continue;

        ReferencePoseData palette{};
        std::fill(std::begin(palette.bones), std::end(palette.bones), math::Matrix4::Identity());
        std::copy_n(refPose.begin(), (std::min)(refPose.size(), std::size(palette.bones)), palette.bones);

        const auto assetGeneration = asset::AssetManager::GetAssetGeneration();
        const auto resourceVersion = resources.GetResetVersion();
        const auto buffer = resources.CreateConstantBuffer(sizeof(palette));
        if (!buffer.IsValid() || resources.GetResetVersion() != resourceVersion) continue;

        /// @note 確保中の再入でモデルが変わったり、別のパレットが先に公開されたりする。
        auto* current = scene.GetComponent<SkinnedMeshRenderer>(id);
        if (scene.GetRenderSceneGeneration() == sceneGeneration
            && asset::AssetManager::GetAssetGeneration() == assetGeneration
            && current && current->model == model && !current->model->referencePoseCB.IsValid()) {
            /// @note 転送中に描画へ再入しても同じモデルの確保を繰り返さないよう、先にハンドルを公開する。
            current->model->referencePoseCB = buffer;
            resources.Update(buffer, &palette, sizeof(palette));
        } else {
            resources.Release(buffer);
        }
    }
}

/// @pre effects はビュー設定の所有するコピーで、シェーダー読込中のコールバックでも生存する。
void PrepareCustomPostProcessing(renderer::ResourceManager& resources,
    const std::vector<renderer::CustomPostProcessSettings>& effects,
    RenderPassHandles& passHandles, renderer::ViewPipelineOptions& options)
{
    const profiler::ProfileScope profile(profiler::ProfilerMarker("RenderSystem::CustomPostProcessSetup", "Rendering"));
    passHandles.customPostProcessShaders.resize(effects.size());
    /// @note 同じ段の効果は RenderGraph の登録順に実行されるため、設定内の順番を保つ。
    options.customPostProcessIndices.reserve(effects.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(effects.size()); ++i) {
        const auto& custom = effects[i];
        if (!custom.enabled) continue;
        /// @note 材質経由のシェーダーとテクスチャは、パスの実行時に解決する。
        if (!custom.shaderPath.empty())
            passHandles.customPostProcessShaders[i] = resources.LoadShader(custom.shaderPath);
        if (!passHandles.customPostProcessShaders[i].IsValid() && custom.materialPath.empty())
            continue;
        switch (custom.stage) {
        case renderer::CustomPassStage::AfterOpaque: options.customAfterOpaqueIndices.push_back(i); break;
        case renderer::CustomPassStage::SceneHDR:    options.customSceneHdrIndices.push_back(i);    break;
        case renderer::CustomPassStage::PostProcess: options.customPostProcessIndices.push_back(i); break;
        }
    }
    /// @note マスクの読み取りパスがない場合、RenderGraph はマスク生成を省く。
    {
        static size_t sAfterOpaque = SIZE_MAX;
        static size_t sSceneHdr    = SIZE_MAX;
        static size_t sPostProcess = SIZE_MAX;
        if (options.customAfterOpaqueIndices.size() != sAfterOpaque
            || options.customSceneHdrIndices.size() != sSceneHdr
            || options.customPostProcessIndices.size() != sPostProcess) {
            sAfterOpaque = options.customAfterOpaqueIndices.size();
            sSceneHdr    = options.customSceneHdrIndices.size();
            sPostProcess = options.customPostProcessIndices.size();
            std::string names;
            for (const auto& custom : effects) {
                names += custom.name;
                names += custom.enabled ? "(on) " : "(off) ";
            }
            FBZZ_LOG_INFO("CustomPass: afterOpaque=%zu sceneHdr=%zu postProcess=%zu / %s",
                          sAfterOpaque, sSceneHdr, sPostProcess,
                          names.empty() ? "(効果なし)" : names.c_str());
        }
    }
}

void PublishDebugSnapshot(const RenderPipeline& pipeline, const RenderPassContext& passCtx,
    const renderer::RenderViewResources& viewTargets, bool graphExecuted, bool passViewerEnabled)
{
    FBZZ_PROFILE_SCOPE("RenderSystem::DebugSnapshot");
    renderer::RenderDebugOverlay::Snapshot dbgSnap;
    dbgSnap.hdrRT           = viewTargets.hdr;
    dbgSnap.ldrRT           = viewTargets.ldr;
    dbgSnap.selectionMaskRT = viewTargets.selectionMask;
    dbgSnap.outlineRT       = viewTargets.outline;
    dbgSnap.gbufferRT       = viewTargets.gbuffer;
    dbgSnap.width           = viewTargets.width;
    dbgSnap.height          = viewTargets.height;
    dbgSnap.planDescription = pipeline.LastPlanDescription();
    for (const auto& profile : pipeline.LastReport().profiles)
        dbgSnap.passTimings.push_back({ profile.name, profile.cpuMilliseconds });

    /// @note 遅延値は出自を保持し、別ビュー・旧 Plan・再生成前の資源へ帰属させない。
    dbgSnap.gpuCurrentView = graphExecuted ? pipeline.LastGpuProfilerView() : renderer::GpuProfilerViewMetadata{};
    dbgSnap.gpuProfiler = passCtx.renderer.GpuProfGetSnapshot();
    std::erase_if(dbgSnap.gpuProfiler.passes, [&](const auto& gp) {
        return !gp.available || !std::isfinite(gp.gpuMs) || gp.gpuMs < 0.0
            || gp.physicalFrameSerial != dbgSnap.gpuProfiler.physicalFrameSerial
            || gp.deviceEpoch != dbgSnap.gpuProfiler.deviceEpoch
            || !renderer::IsGpuProfilerViewCompatible(gp.metadata, dbgSnap.gpuCurrentView, 8u);
    });
    dbgSnap.gpuProfiler.available = dbgSnap.gpuProfiler.available && !dbgSnap.gpuProfiler.passes.empty();
    for (const auto& gp : dbgSnap.gpuProfiler.passes)
        dbgSnap.gpuPassTimings.push_back({ gp.name, gp.gpuMs });

    dbgSnap.renderStats.totalObjects    = passCtx.statsTotalObjects;
    dbgSnap.renderStats.frustumCulled   = passCtx.statsFrustumCulled;
    dbgSnap.renderStats.occlusionCulled = passCtx.statsOcclusionCulled;
    dbgSnap.renderStats.frustumCullingEnabled = passCtx.frustumCullingEnabled;
    dbgSnap.renderStats.occlusionCullingEnabled = passCtx.occlusionCullingEnabled;
    dbgSnap.renderStats.distanceCulled    = passCtx.statsDistanceCulled;
    dbgSnap.renderStats.smallObjectCulled = passCtx.statsSmallObjectCulled;
    dbgSnap.renderStats.gpuVisibilityUnknownObjects = passCtx.statsGpuVisibilityUnknownObjects;
    dbgSnap.renderStats.drawCalls       = passCtx.statsDrawCalls;
    dbgSnap.renderStats.vertexCount     = passCtx.statsVertexCount;
    dbgSnap.renderStats.triangleCount   = passCtx.statsTriangleCount;
    dbgSnap.renderStats.skinningVertexCount = passCtx.statsSkinningVertexCount;
    dbgSnap.renderStats.skinningDispatchCount = passCtx.statsSkinningDispatchCount;
    dbgSnap.renderStats.skinningPaletteUploadBytes = passCtx.statsSkinningPaletteUploadBytes;
    dbgSnap.renderStats.skinningPaletteUploads = passCtx.statsSkinningPaletteUploads;
    dbgSnap.renderStats.skinningDemandMode = passCtx.statsSkinningDemandMode;
    dbgSnap.renderStats.skinningDemandCandidates = passCtx.statsSkinningDemandCandidates;
    dbgSnap.renderStats.skinningDemandRequired = passCtx.statsSkinningDemandRequired;
    dbgSnap.renderStats.skinningDemandPrepared = passCtx.statsSkinningDemandPrepared;
    dbgSnap.renderStats.skinningDemandSubmitted = passCtx.statsSkinningDemandSubmitted;
    dbgSnap.renderStats.skinningDemandSkipped = passCtx.statsSkinningDemandSkipped;
    dbgSnap.renderStats.shadowDrawCalls     = passCtx.statsShadowDrawCalls;
    dbgSnap.renderStats.staticShadowUpdateMask = passCtx.staticShadowCache.updateMask;
    dbgSnap.renderStats.staticShadowReadyMask = passCtx.staticShadowCache.readyMask;
    dbgSnap.renderStats.shadowTriangleCount = passCtx.statsShadowTriangleCount;
    dbgSnap.renderStats.instancedBatches    = passCtx.statsInstancedBatches;
    dbgSnap.renderStats.instancedDrawsSaved = passCtx.statsInstancedDrawsSaved;

    renderer::RenderDebugOverlay::UpdateSnapshot(dbgSnap, passViewerEnabled);
}

}

void RenderSystem(Scene& scene,
                  renderer::IRenderer& renderer,
                  renderer::ResourceManager& resources,
                  const renderer::Camera& inputCamera,
                  renderer::ResourceHandle<renderer::RenderTargetTag> outputRT,
                  const renderer::RenderSettings* settings,
                  fbzz::LayerMask cullingMask,
                  const RenderSystemUIOptions* uiOptions,
                  const physics::World* physicsWorld,
                  const CameraCullingSettings* cullingSettings,
                  renderer::RenderPassCapture* capture,
                  RenderFrameGeometryCache* frameGeometry,
                  std::shared_ptr<renderer::SurfacePatchSelectionCacheOwner> surfacePatchPlans)
{
    FBZZ_PROFILE_SCOPE("RenderSystem");
    const uint32_t viewKey = uiOptions ? static_cast<uint32_t>(uiOptions->targetView) + 1u : 0u;
    auto timelineContext = profiler::frame_timeline::CurrentContext();
    timelineContext.viewId = viewKey;
    timelineContext.resourceFrameSerial = resources.FrameStamp();
    profiler::frame_timeline::ContextScope timelineViewContext(timelineContext);
    profiler::frame_timeline::Scope timelineView(profiler::frame_timeline::Event::VIEW);
    std::optional<profiler::frame_timeline::Scope> postTimeline;
    /// @note 後続のローカル変数の破棄まで計測するため、実行時の計測と Cleanup を先に宣言する。
    cpu_trace::ViewScope cpuTrace(resources.GetInstanceId(), resources.GetResetVersion(), resources.FrameStamp(),
        scene.GetRenderSceneGeneration(), outputRT.id, outputRT.gen);

    std::optional<profiler::ProfileScope> cleanupProfile;
    const auto viewSettingsProfile = profiler::Profiler::BeginScope(
        profiler::ProfilerMarker("RenderSystem::ResolveViewSettings", "Rendering"));

    /// @note 呼び出し側で指定しなければ、Standalone でもシーンのメインカメラの設定を使う。
    const CameraCullingSettings resolvedCulling =
        cullingSettings ? *cullingSettings : ResolveGameCullingSettings(scene);
    const auto viewSettings = render_view_detail::ResolveViewSettings(scene, inputCamera, settings);
    const auto& camera = viewSettings.camera;
    const auto& rs = viewSettings.settings;
    const auto activeIblSource = viewSettings.iblSource;
    profiler::Profiler::EndScope(viewSettingsProfile);

    const bool experimentalRayTracingEnabled = core::DeveloperMode::IsEnabled();

    /// @note UI の通知は Script 後に確定する。描画資源の初期化失敗でも CPU 入力を止めない。
    if (uiOptions && uiOptions->enabled && uiOptions->context
        && uiOptions->targetView == UIRenderTargetView::GameViewport) {
        const auto* target = resources.Get(outputRT);
        const float width = uiOptions->viewportWidth > 0.0f ? uiOptions->viewportWidth
            : static_cast<float>(target ? target->GetWidth() : renderer.GetWidth());
        const float height = uiOptions->viewportHeight > 0.0f ? uiOptions->viewportHeight
            : static_cast<float>(target ? target->GetHeight() : renderer.GetHeight());
        UpdateUIInteraction(scene, *uiOptions->context,
            {width, height, uiOptions->mouseInCanvasSpace, camera.GetViewProjection(),
             camera.m_position, camera.m_rotation, uiOptions->gameplayInput,
             !outputRT.IsValid() || resources.GetDepthTexture(outputRT).IsValid()}, uiOptions->mousePressed);
    }

    const auto sharedResourcesProfile = profiler::Profiler::BeginScope(
        profiler::ProfilerMarker("RenderSystem::SharedResourceSetup", "Rendering"));
    auto& renderResources = resources.Rendering();
    renderResources.SetExperimentalRayTracingEnabled(experimentalRayTracingEnabled);
    auto& shared = renderResources.Shared();
    if (shared.Prepare(resources, rs)) {
        ReleaseSkinningComputeCaches();
        renderer::ReleaseInstanceBatchCaches(resources);
        ReleaseDecalMaterialCache();
        ReleaseCustomPassMaterialCache();
    }
    cpuTrace.SetViewId(viewKey);
    auto& viewTargets = renderResources.View(viewKey);
    profiler::Profiler::EndScope(sharedResourcesProfile);

    const auto viewResourcesProfile = profiler::Profiler::BeginScope(
        profiler::ProfilerMarker("RenderSystem::ViewResourceSetup", "Rendering"));
    const bool viewResourcesReady = renderResources.PrepareView(viewTargets, renderer, outputRT, rs);
    profiler::Profiler::EndScope(viewResourcesProfile);
    if (!viewResourcesReady) {
        const auto failedPlan = renderer::PrepareViewRenderPlan(resources, renderer, rs,
            viewTargets, shared, {}, experimentalRayTracingEnabled);
        FBZZ_LOG_ERROR("RenderSystem: %s", renderer::DescribeRenderPlanReason(failedPlan.failureReason));
        return;
    }
    const uint32_t nativeW = viewTargets.nativeWidth;
    const uint32_t nativeH = viewTargets.nativeHeight;
    const bool needsUpscale = viewTargets.needsUpscale;
    const uint32_t punctualShadowRes = (std::max)(rs.shadow.punctualMapResolution, 64u);
    auto& upscaleSrcRT            = viewTargets.upscaleSrc;
    auto& gtaoBlur                = viewTargets.gtaoBlur;
    auto& contactShadowResult     = viewTargets.contactShadowResult;
    ValidateReferencePoseBuffers(scene, resources);
    auto& clusterCB = shared.clusterCB;
    auto& clusterLinearCB = shared.clusterLinearCB;

    /// @note ライト収集とシャドウ範囲計算はシーン全体を走査するため、独立して計測する。
    profiler::Profiler::BeginSample(
        profiler::ProfilerMarker("RenderSystem::LightingSetup", "Rendering"));

    auto lighting = ExtractRenderLights(scene, camera, rs, punctualShadowRes, experimentalRayTracingEnabled);
    auto& lightData = lighting.lightData;
    auto& dirCastShadows = lighting.dirCastShadows;
    auto& dirShadowBias = lighting.dirShadowBias;
    auto& dirShadowStrength = lighting.shadowStrength;
    auto& punctualLights = lighting.punctualLights;
    auto& legacySourceRadius = lighting.legacySourceRadius;
    auto& punctualViews = lighting.punctualShadowViews;
    auto& punctualViewCount = lighting.punctualShadowViewCount;
    auto& legacyShadowSlots = lighting.legacyShadowSlots;
    auto& cookieViews = lighting.lightCookieViews;
    auto& cookieViewCount = lighting.lightCookieViewCount;
    auto& legacyCookieSlots = lighting.legacyCookieSlots;
    auto& skyCloudShadowStrength = lighting.cloudShadowStrength;
    auto& skyCloudShadowCoverage = lighting.cloudShadowCoverage;
    auto& skyCloudShadowScale = lighting.cloudShadowScale;
    auto& skyCloudShadowSpeed = lighting.cloudShadowSpeed;
    math::Vector3 lightDir = lightData.lightDir.Normalized();
    const auto shadowPreparation = PrepareViewShadows(scene, renderer, resources, camera, rs,
        shared, viewTargets, lighting, lightDir, cullingMask);
    const auto& shadowBounds = shadowPreparation.bounds;
    const auto& shadows = shadowPreparation.shadows;
    auto& shadowAtlas = viewTargets.staticShadowAtlas;
    auto& shadowCaches = viewTargets.shadowCascadeCaches;
    const uint32_t staticShadowCascadeMask = shadowPreparation.staticMask;
    const uint32_t reuseShadowCascadeMask = shadowPreparation.reuseMask;
    const auto& cascades = shadows.cascades;
    const int cascadeCount = shadows.count;
    const auto& lightVP = shadows.lightVP;
    const auto& lightView = shadows.lightView;
    const auto& lightPos = shadows.lightPos;

    RenderPassHandles passHandles{};
    renderResources.BindPassHandles(viewTargets, passHandles);
    renderer::ViewPipelineOptions pipelineOptions;
    pipelineOptions.renderPlan = renderer::PrepareViewRenderPlan(
        resources, renderer, rs, viewTargets, shared, passHandles, experimentalRayTracingEnabled);
    const auto& renderPlan = pipelineOptions.renderPlan;
    if (!renderPlan.IsValid()) {
        profiler::Profiler::EndSample();
        FBZZ_LOG_ERROR("RenderSystem: %s", renderer::DescribeRenderPlanReason(renderPlan.failureReason));
        return;
    }
    const auto& opaquePlan = renderPlan.rasterPlan;
    const bool screenSpaceReady = opaquePlan.HasScreenSpaceInputs();

    const bool ssaoEnabled =
        screenSpaceReady &&
        rs.postProcess.ambientOcclusion.enabled &&
        shared.ssaoShader.IsValid() &&
        shared.ssaoBlurShader.IsValid() &&
        viewTargets.ssaoRaw.IsValid() &&
        viewTargets.ssaoBlur.IsValid();

    const bool previewSelection = uiOptions && std::any_of(uiOptions->instancePreviews.begin(), uiOptions->instancePreviews.end(),
        [](const auto& preview) { return preview.erase && preview.data && !preview.data->placements.empty(); });
    const bool selectionOutlineEnabled =
        rs.showSelectionOutline && (!rs.selectedObjects.empty() || previewSelection) &&
        viewTargets.selectionMask.IsValid() && shared.selectionMaskPso.IsValid() &&
        shared.selectionOutlineShader.IsValid();

    /// @note 要求のないビューでは全画面クリアを省き、無効化の理由を診断に残す。
    const char* objectMaskOffReason = nullptr;
    const bool objectMaskEnabled = [&]() {
        if (!viewTargets.objectMask.IsValid())      { objectMaskOffReason = "objectMaskRT 未作成"; return false; }
        if (!shared.selectionMaskPso.IsValid())  { objectMaskOffReason = "selectionMaskPso 未作成"; return false; }
        if (!shared.objectMaskShader.IsValid() && !shared.objectMaskSkinnedShader.IsValid()) {
            objectMaskOffReason = "Pipeline/Mask のシェーダーが両方とも読めない";
            return false;
        }
        for (const renderer::RenderObjectMaskRequest& request : rs.objectMaskRequests)
            if (renderer::IsObjectMaskRequestLive(request, Time::frameCount)) return true;
        objectMaskOffReason = rs.objectMaskRequests.empty()
            ? "申告が 1 件も届いていない (RenderSettings の実体違い)"
            : "申告はあるが全部フレーム落ち (提出が描画より後)";
        return false;
    }();
    {
        static const char* sLastReason = "";
        const char* reason = objectMaskEnabled ? "(有効)" : objectMaskOffReason;
        if (reason && reason != sLastReason) {
            sLastReason = reason;
            /// @note 設定のコピー元を比較して、マスクの要求先と描画時の参照先を照合する。
            const void* readFrom = static_cast<const void*>(settings);
            const void* active   = static_cast<const void*>(
                core::Application::Get().GetActiveRenderSettings());
            FBZZ_LOG_INFO("ObjectMask: %s (requests=%zu / 読み手=%p 申告先=%p %s)",
                          reason, rs.objectMaskRequests.size(), readFrom, active,
                          readFrom == active ? "一致" : "不一致");
        }
    }

    /// @note 描画設定を受け取った場合に限り、無効な設定の診断を変化時に記録する。
    if (settings != nullptr) {
        static std::string sLastInertReport;
        std::string report;
        for (const renderer::InertSetting& issue : renderer::CollectInertSettings(rs)) {
            report += issue.label;
            report += " / ";
        }
        if (report != sLastInertReport) {
            sLastInertReport = report;
            if (!report.empty()) {
                FBZZ_LOG_WARN("Pipeline: 有効ですが現在のパイプラインでは無視される設定があります "
                              "-> %s(Project Settings > Rendering > Pipeline を確認してください)",
                              report.c_str());
            }
        }
    }
    profiler::Profiler::EndSample();

    PrepareCustomPostProcessing(resources, rs.postProcess.customEffects, passHandles, pipelineOptions);

    PrepareReferencePoseBuffers(scene, resources);
    /// @note LoadTexture が IBL テクスチャをキャッシュするため、ビューごとの参照解決で再読込しない。
    {
        FBZZ_PROFILE_SCOPE("RenderSystem::ResolveIblTextures");
        if (!rs.ibl.irradiancePath.empty())
            passHandles.iblIrradiance = asset::StreamedTextureResolver::Engine().ResolveGpu(resources, rs.ibl.irradiancePath);
        if (!rs.ibl.prefilterPath.empty())
            passHandles.iblPrefilter = asset::StreamedTextureResolver::Engine().ResolveGpu(resources, rs.ibl.prefilterPath);
    }
    const auto passContextProfile = profiler::Profiler::BeginScope(
        profiler::ProfilerMarker("RenderSystem::PassContextSetup", "Rendering"));
    /// @note Reversed-Z の遠平面は z 行から直接抽出できる。通常 Z では w 行との差で遠距離の精度が落ちる。
    /// @see Docs/design/view-culling.md
    const math::Frustum cameraFrustum = math::Frustum::FromViewProjection(camera.GetGpuViewProjection());
    const math::Frustum lightFrustum  = math::Frustum::FromViewProjection(lightVP);
    renderer::OcclusionCuller occlusionCuller;

    RenderPassContext passCtx{
        scene, renderer, resources, camera, rs,
        outputRT, cullingMask, passHandles
    };
    /// @note パッチ選択の借用期間中は値渡しの所有者が生存し、所有者がなければビュー内で選択する。
    renderer::SurfacePatchSelectionCacheOwner localSurfacePatchPlans;
    const auto surfacePatchLease = (surfacePatchPlans ? *surfacePatchPlans : localSurfacePatchPlans).Borrow(
        {resources.GetInstanceId(), resources.GetResetVersion(), scene.GetRenderSceneGeneration(),
         viewKey, outputRT.id, outputRT.gen}, passCtx.surfacePatchSelectionCache);
    passCtx.frustumCullingEnabled   = resolvedCulling.frustumCulling;
    passCtx.experimentalRayTracingEnabled = experimentalRayTracingEnabled;
    passCtx.occlusionCullingEnabled = resolvedCulling.occlusionCulling;
    passCtx.cullingBoundsPadding    = resolvedCulling.cullingBoundsPadding;
    passCtx.cullMaxDistance         = resolvedCulling.maxDrawDistance;
    passCtx.cullDistanceSpherical   = resolvedCulling.cullDistanceSpherical;
    passCtx.smallObjectScreenHeight = resolvedCulling.smallObjectScreenHeight;
    for (int i = 0; i < kCullLayerCount; ++i) {
        passCtx.cullLayerDistances[i] = resolvedCulling.layerCullDistances[i];
        if (resolvedCulling.layerCullDistances[i] > 0.0f)
            passCtx.hasLayerCullDistances = true;
    }
    /// @note 極小オブジェクト判定の射影スケール 1 / tan(fovY / 2) をビュー単位で求める。
    passCtx.cullProjScaleY    = camera.GetProjectionMatrix().m[1][1];
    passCtx.cullOrthographic  =
        camera.m_projection == renderer::ProjectionMode::Orthographic;
    passCtx.cullCameraForward = camera.GetForward();
    passCtx.width                   = viewTargets.width;
    passCtx.height                  = viewTargets.height;

    passCtx.uiOptions               = uiOptions;
    passCtx.directionalShadowBakeEnabled = rs.shadowEnabled && lighting.dirShadowBakeLight != EntityID::INVALID;
    passCtx.staticShadowCache.atlas = shadowAtlas.Handle();
    passCtx.staticShadowCache.cascadeMask = staticShadowCascadeMask;
    passCtx.staticShadowCache.reuseMask = reuseShadowCascadeMask;
    for (int index = 0; index < cascadeCount; ++index)
        passCtx.staticShadowCache.contentKeys[index] = shadowCaches[index].contentKey;
    passCtx.outputWidth             = nativeW;
    passCtx.outputHeight            = nativeH;
    passCtx.chainOutputRT           = (needsUpscale && upscaleSrcRT.IsValid()) ? upscaleSrcRT : outputRT;
    /// @note 8 フレーム周期の Halton(2, 3) を ±0.5 ピクセルに写し、無効時はジッターを適用しない。
    if (rs.IsTaaActive() && viewTargets.width > 0u && viewTargets.height > 0u) {
        constexpr uint32_t kTaaJitterPeriod = 8u;
        const uint32_t sampleIndex = viewTargets.taaFrameIndex % kTaaJitterPeriod + 1u;
        const float offsetPxX = HaltonRadicalInverse(sampleIndex, 2u) - 0.5f;
        const float offsetPxY = HaltonRadicalInverse(sampleIndex, 3u) - 0.5f;
        /// @note ピクセル → NDC。NDC の Y は上向きなので符号を反転する。
        passCtx.taaJitterNdcX =  2.0f * offsetPxX / static_cast<float>(viewTargets.width);
        passCtx.taaJitterNdcY = -2.0f * offsetPxY / static_cast<float>(viewTargets.height);
        ++viewTargets.taaFrameIndex;
    } else {
        viewTargets.taaFrameIndex = 0u;
        viewTargets.taaHistoryValid = false;
    }
    passCtx.selectionOutlineEnabled = selectionOutlineEnabled;
    passCtx.objectMaskEnabled      = objectMaskEnabled;
    /// @note UI と 3D で選択マスクを共有し、UI の座標は出力先の実寸でクリップ空間へ変換する。
    if (selectionOutlineEnabled && uiOptions && uiOptions->enabled && uiOptions->context) {
        passCtx.appendUISelectionMask = [&]() {
            const float uiWidth = uiOptions->viewportWidth > 0.0f
                ? uiOptions->viewportWidth
                : static_cast<float>(nativeW);
            const float uiHeight = uiOptions->viewportHeight > 0.0f
                ? uiOptions->viewportHeight
                : static_cast<float>(nativeH);
            const RenderSelectionLookup selection(passCtx.settings.selectedObjects);
            UISelectionMaskSystem(
                scene, renderer, resources, *uiOptions->context,
                uiWidth, uiHeight,
                [&](GameObject& go) { return selection.Contains(go.GetID()); },
                camera.m_position, camera.m_rotation, camera.GetViewProjection(),
                uiOptions->targetView);
        };
    }
    passCtx.lightData               = lightData;
    passCtx.lightVP                 = lightVP;
    passCtx.lightView               = lightView;
    passCtx.lightEyePos             = lightPos;
    passCtx.isDeferred              = opaquePlan.UsesDeferredLighting();
    passCtx.ssaoEnabled             = ssaoEnabled;
    passCtx.gbufferDepthReady       = screenSpaceReady;

    /// @note 前方描画用の AO スロットを GTAO と SSAO で共有する。
    /// @note 遅延描画でも半透明などの前方描画には画面空間の遮蔽が必要。
    float screenAoStrength = 0.0f;
    float screenContactShadowStrength = 0.0f;

    if (screenSpaceReady && rs.IsGtaoActive() && gtaoBlur.IsValid()) {
        passCtx.screenAoTexture = gtaoBlur;
        /// @note GTAO の出力は gtaoIntensity を織り込み済み。ここで再度掛けると二重になる。
        screenAoStrength = 1.0f;
    } else if (ssaoEnabled) {
        passCtx.screenAoTexture = viewTargets.ssaoBlur;
        screenAoStrength = rs.postProcess.ambientOcclusion.intensity;
    }
    if (screenSpaceReady && rs.contactShadow.enabled && contactShadowResult.IsValid()) {
        passCtx.screenContactShadowTexture = contactShadowResult;
        /// @note 強度はマスク生成 CS 側で織り込み済み。ここは「適用するか」だけを決める。
        screenContactShadowStrength = 1.0f;
    }

    /// @note 点・スポットライト数に上限があるレガシー経路は、資源確保失敗やプレビュー時に使う。
    const bool punctualBufferReady =
        passHandles.punctualLightBuffer.IsValid() && clusterCB.IsValid();
    const bool canCullClusters = renderPlan.clusteredLighting;

    ClusterLightMode clusterMode = ClusterLightMode::Legacy;
    if (punctualBufferReady && !rs.IsUnlit()) {
        clusterMode = canCullClusters ? (rs.clustered.manyLights ? ClusterLightMode::SAMPLED : ClusterLightMode::Clustered)
                                      : ClusterLightMode::Linear;
    }
    const bool clusteredEnabled = UsesClusterLightIndices(clusterMode);

    passCtx.punctualLights    = std::move(punctualLights);
    passCtx.rayLights = std::move(lighting.rayLights);
    passCtx.rayLightsComplete = lighting.rayLightsComplete;
    passCtx.clusterLightMode  = clusterMode;
    /// @note 霧のフレーム間状態は描画中のビューが持つ (SceneView / GameView で混ざらないように)。
    passCtx.froxelFogState    = &viewTargets.froxelState;
    passCtx.clusterDebugHeatmap = clusteredEnabled && rs.clustered.debugHeatmap;

    if (punctualBufferReady) {
        FBZZ_PROFILE_SCOPE("RenderSystem::LightBufferUpload");
        if (!passCtx.punctualLights.empty()) {
            resources.Update(passHandles.punctualLightBuffer, passCtx.punctualLights.data(),
                             passCtx.punctualLights.size() * sizeof(PunctualLightGPU));
        }

        /// @note log(viewZ) を線形変換して近面を 0、クラスタ遠面を GridZ に写し、手前の深度を細かく分割する。
        const float nearZ = (std::max)(camera.m_near, 0.01f);
        const float farZ  = (std::max)((std::min)(camera.m_far, rs.clustered.maxDistance), nearZ * 2.0f);
        const float logRatio = std::log(farZ / nearZ);

        ClusterConstantsCB clusterData{};
        clusterData.clusterTilePx[0] = static_cast<float>(viewTargets.width) / static_cast<float>(kClusterGridX);
        clusterData.clusterTilePx[1] = static_cast<float>(viewTargets.height) / static_cast<float>(kClusterGridY);
        clusterData.clusterSliceScale = static_cast<float>(kClusterGridZ) / logRatio;
        clusterData.clusterSliceBias  = -static_cast<float>(kClusterGridZ) * std::log(nearZ) / logRatio;
        clusterData.clusterLightMode  = static_cast<uint32_t>(clusterMode);
        clusterData.punctualLightCount = static_cast<uint32_t>(passCtx.punctualLights.size());
        clusterData.clusterDebugMode  = passCtx.clusterDebugHeatmap ? 1u : 0u;
        if (clusterMode == ClusterLightMode::SAMPLED) {
            const uint32_t phase = rs.IsTaaActive() ? static_cast<uint32_t>(Time::frameCount) & 0x03FFFFFFu : 0u;
            clusterData.manyLightControl = (phase << 6u) | static_cast<uint32_t>(std::clamp(rs.clustered.manyLightSamples, 1, 32));
        }
        resources.Update(clusterCB, &clusterData, sizeof(ClusterConstantsCB));

        /// @note 別視点にはメインカメラのクラスタ割り当てを使わず、ライトバッファがある場合だけ全数走査する。
        ClusterConstantsCB linearData = clusterData;
        if (clusterMode != ClusterLightMode::Legacy)
            linearData.clusterLightMode = static_cast<uint32_t>(ClusterLightMode::Linear);
        linearData.clusterDebugMode = 0u;
        linearData.manyLightControl = 0u;
        resources.Update(clusterLinearCB, &linearData, sizeof(ClusterConstantsCB));
    }
    passCtx.cameraFrustum           = &cameraFrustum;
    passCtx.lightFrustum            = &lightFrustum;
    passCtx.occlusionCuller         = &occlusionCuller;
    passCtx.physicsWorld  = physicsWorld;
    passCtx.environmentResources = &shared.sEnvironmentResources;
    passCtx.cloudShadowStrength = skyCloudShadowStrength;
    passCtx.cloudShadowCoverage = skyCloudShadowCoverage;
    passCtx.cloudShadowScale    = skyCloudShadowScale;
    passCtx.cloudShadowSpeed    = skyCloudShadowSpeed;
    passCtx.cloudShadowTime     = Time::time;
    /// @note 影を無効にした場合は、シェーダーの PCF 処理も省く。
    passCtx.shadowStrength =
        (rs.shadowEnabled && dirCastShadows) ? dirShadowStrength : 0.0f;
    /// @note 単一カスケード相当のバイアス。カスケードごとの値は ShadowCascade::biasNDC が持つ。
    passCtx.shadowBiasNDC  = cascades[0].biasNDC;
    passCtx.shadowCascadeCount = cascadeCount;
    for (int i = 0; i < cascadeCount; ++i)
        passCtx.shadowCascades[i] = cascades[i];

    passCtx.punctualShadowViewCount  = punctualViewCount;
    passCtx.punctualShadowResolution = punctualShadowRes;
    for (int i = 0; i < punctualViewCount; ++i)
        passCtx.punctualShadowViews[i] = punctualViews[i];
    for (int i = 0; i < kMaxLegacyPunctualLights; ++i) {
        passCtx.legacyShadowSlots[i] = legacyShadowSlots[i];
        passCtx.legacyCookieSlots[i] = legacyCookieSlots[i];
    }

    passCtx.lightCookieViewCount = cookieViewCount;
    for (int i = 0; i < cookieViewCount; ++i)
        passCtx.lightCookieViews[i] = cookieViews[i];

    passCtx.legacyShapedLightCount = 0;
    for (const PunctualLightGPU& light : passCtx.punctualLights) {
        if (passCtx.legacyShapedLightCount >= kMaxLegacyShapedLights) break;
        const bool shaped = light.type == static_cast<uint32_t>(PunctualLightType::Area)
                         || light.type == static_cast<uint32_t>(PunctualLightType::Sphere)
                         || light.type == static_cast<uint32_t>(PunctualLightType::Tube);
        if (!shaped) continue;
        passCtx.legacyShapedLights[passCtx.legacyShapedLightCount++] = light;
    }
    for (int i = 0; i < kMaxLegacyPunctualLights; ++i)
        passCtx.legacySourceRadius[i] = legacySourceRadius[i];
    profiler::Profiler::EndScope(passContextProfile);

    /// @note プローブも同じ入力を読むため、グラフ外の捕捉より前に変形と抽出を完了する。
    cpuTrace.SetPhase(cpu_trace::Phase::SKIN);
    skinning_demand_detail::GpuOutputDemandScope gpuOutputDemand(passCtx);
    {
        /// @note 出力要求の件数を診断するときに、このビューの呼び出しを識別する。
        skinning_demand_detail::OutputDemandScope demandScope(passCtx);
        profiler::frame_timeline::Scope timelineSkinning(profiler::frame_timeline::Event::SKINNING);
        ExecuteSkinningComputePass(passCtx);
    }
    RequestWaterfallFluidForView(scene, camera, cameraFrustum, passCtx.height);
    cpuTrace.SetPhase(cpu_trace::Phase::EXTRACT);
    RenderFrameGeometryCache* reusableGeometry = frameGeometry;
    if (reusableGeometry && !CanShareFrameGeometry(scene)) {
        /// @note ビュー間で描画入力を書き換え得る状態になったら次回抽出をやり直す。公開済みの入力は共有所有権を保持する。
        reusableGeometry->geometry.reset();
        reusableGeometry->deferredItemGeometry.reset();
        reusableGeometry = nullptr;
    }
    material_demand_detail::BindingDemandScope materialDemand(passCtx);
    {
        profiler::frame_timeline::Scope timelineExtract(profiler::frame_timeline::Event::EXTRACT);
        ExtractRenderScene(passCtx, reusableGeometry);
    }
    if (passCtx.directionalShadowBakeEnabled) {
        PrepareDirectionalShadowBakeForRender(passCtx, lighting.dirShadowBakeLight, shadowBounds, lightDir,
            dirShadowBias, shadows);
    }
    cpuTrace.SetPhase(cpu_trace::Phase::ENVIRONMENT_PIPELINE);

    PrepareViewEnvironment(passCtx, viewTargets, activeIblSource, screenAoStrength, screenContactShadowStrength);

    const auto pipelineExtensionsProfile = profiler::Profiler::BeginScope(
        profiler::ProfilerMarker("RenderSystem::PipelineExtensions", "Rendering"));
    RenderPipeline pipeline(std::move(viewTargets.pipeline));
    renderer::DebugDrawCapture scriptGizmoCapture;
    const auto extensions = MakeViewPipelineExtensions(pipeline, passCtx, scriptGizmoCapture, uiOptions);
    profiler::Profiler::EndScope(pipelineExtensionsProfile);
    renderer::BuildViewPipeline(pipeline, passCtx, viewTargets, shared, pipelineOptions, extensions);
    material_demand_detail::CompleteBindings(passCtx);

    {
        FBZZ_PROFILE_SCOPE("RenderSystem::ScriptPreRender");
        ForEachActiveRenderScript(scene, [](Script& script) {
            script.ExecuteProfiledCallback(&Script::OnPreRender, ScriptCallbackKind::PRE_RENDER, "OnPreRender");
        });
    }


    /// @note GPU query 領域は renderer の物理フレームが所有する。ビュー開始で再初期化しない。
    {
        FBZZ_PROFILE_SCOPE("RenderSystem::GpuProfilerSetup");
        renderer.GpuProfCollect();

        pipeline.SetGpuProfilerHooks(
            [&](std::string_view name) { renderer.GpuProfBeginPass(name.data()); },
            [&](std::string_view name) { renderer.GpuProfEndPass(name.data()); }
        );
    }

    renderer::GpuProfilerViewMetadata gpuView;
    gpuView.applicationFrameSerial = resources.FrameStamp();
    gpuView.viewId = viewKey;
    gpuView.sceneGeneration = scene.GetRenderSceneGeneration();
    gpuView.resourceEpoch = resources.GetResetVersion();
    gpuView.outputId = outputRT.id;
    gpuView.outputGeneration = outputRT.gen;
    gpuView.width = viewTargets.width;
    gpuView.height = viewTargets.height;
    cpuTrace.SetPhase(cpu_trace::Phase::GRAPH);
    material_demand_detail::CompleteBindings(passCtx);
    bool graphExecuted = false;
    {
        profiler::frame_timeline::Scope timelineGraph(profiler::frame_timeline::Event::GRAPH);
        graphExecuted = pipeline.Execute(passCtx, capture, &gpuView, &viewTargets.renderPlan);
    }
    postTimeline.emplace(profiler::frame_timeline::Event::POST_CLEANUP);
    cpuTrace.SetPhase(cpu_trace::Phase::POST_CLEANUP);

    if (capture) {
        FBZZ_PROFILE_SCOPE("RenderSystem::CaptureFinish");
        capture->Finish(pipeline.LastReport(), renderer.GpuProfGetSnapshot(), pipeline.LastGpuProfilerView());
    }
    assert(graphExecuted);
    (void)graphExecuted;
    if (shadowAtlas.IsValid()) {
        for (int index = 0; index < renderer::kMaxShadowCascades; ++index) {
            shadowCaches[index].valid = graphExecuted && (passCtx.staticShadowCache.readyMask & (1u << index));
            shadowCaches[index].contentKey = passCtx.staticShadowCache.contentKeys[index];
        }
    }
    /// @note TAA が今回書いた履歴と、自動露出が処理したリセット世代を次の描画へ引き継ぐ。
    viewTargets.taaFlip                 = passHandles.taaFlip;
    viewTargets.exposureResetGeneration = passHandles.exposureResetGeneration;

    {
        FBZZ_PROFILE_SCOPE("RenderSystem::ScriptPostRender");
        ForEachActiveRenderScript(scene, [](Script& script) {
            script.ExecuteProfiledCallback(&Script::OnPostRender, ScriptCallbackKind::POST_RENDER, "OnPostRender");
        });
    }

    PublishDebugSnapshot(pipeline, passCtx, viewTargets, graphExecuted, rs.passViewerEnabled);
    cleanupProfile.emplace(profiler::ProfilerMarker("RenderSystem::Cleanup", "Rendering"));
    scene.ClearUserRenderPasses();
    pipeline.BeginBuild();
    viewTargets.pipeline = std::move(static_cast<renderer::RenderPipeline&>(pipeline));

}

}
