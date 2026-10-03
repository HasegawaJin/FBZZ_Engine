/// @file    RayTracingPipeline.cpp
/// @brief   交差診断の能力ゲートと資源登録を行う。
/// @author  Hasegawa Jin
/// @date    2026-09-30
#include <Graphics/Pipeline/RayTracingPipeline.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Graphics/Passes/RayTracing/RayDebugPass.hpp>
#include <Graphics/Passes/RayTracing/RayReflectionPass.hpp>
#include <Graphics/Passes/RayTracing/RayReflectionReconstructionPass.hpp>
#include <Graphics/Renderer/OpaqueRenderPlan.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/IStructuredBuffer.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::renderer {
bool IsRayDiffuseIndirectReady(const RenderPassContext& context, const RenderViewResources& view)
{
    auto& resources = context.resources;
    const auto* irradiance = resources.Get(context.handles.iblIrradiance);
    const auto& data = view.advancedGraphicsSnapshot;
    if (!view.advancedGraphicsSnapshotValid || !resources.Get(context.handles.advancedGraphicsCB)
        || !irradiance || irradiance->GetBindlessIndex() == INVALID_BINDLESS_INDEX
        || !std::isfinite(data.iblIntensity) || data.iblIntensity <= 0
        || !std::isfinite(data.iblDiffuseScale) || data.iblDiffuseScale < 0) return false;
    for (size_t slot = 0; slot < 2; ++slot) {
        const auto& volume = data.probeVolumes[slot];
        if (!std::isfinite(volume.intensity)) return false;
        if (volume.intensity <= 0) continue;
        const auto* texture = resources.Get(context.handles.lightProbeSH[slot]);
        if (!texture || texture->GetBindlessIndex() == INVALID_BINDLESS_INDEX
            || !volume.grid[0] || !volume.grid[1] || !volume.grid[2]
            || texture->GetWidth() != volume.grid[0] || texture->GetHeight() != volume.grid[1]
            || texture->GetDepth() / 7u != volume.grid[2] || texture->GetDepth() % 7u != 0
            || !std::isfinite(volume.fade) || volume.fade < 0 || !std::isfinite(volume.normalBias)) return false;
        for (float value : {volume.boxMin.x, volume.boxMin.y, volume.boxMin.z})
            if (!std::isfinite(value)) return false;
        for (float value : {volume.invSize.x, volume.invSize.y, volume.invSize.z})
            if (!std::isfinite(value) || value <= 0) return false;
    }
    return true;
}

bool IsRayDebugView(ViewMode mode)
{
    return mode == ViewMode::RayHitDistance || mode == ViewMode::RayGeometricNormal
        || mode == ViewMode::RayInstanceId;
}

bool PrepareRayDebugView(RenderPassContext& context, RenderViewResources& view, RenderSharedResources& shared)
{
    auto& state = view.rayDebug;
    state.gpu = {};
    state.scene = {};
    if (!context.experimentalRayTracingEnabled) return false;
    shared.rayGeometry.Trim(context.resources, context.frameStamp);
    if (!IsRayDebugView(context.settings.viewMode) || !context.renderer.GetCapabilities().inlineRayQuery
        || !context.renderScene || !context.width || !context.height) return false;
    RayDebugConstants cameraConstants{};
    if (!MakeRayDebugCameraConstants(context.camera, cameraConstants)) return false;
    if (!context.resources.Get(shared.rayDebugShader))
        shared.rayDebugShader = context.resources.LoadShader("Assets/Shaders/RayTracing/RayDebug.cs.hlsl");
    if (!context.resources.Get(shared.rayDebugShader) || !context.resources.Get(shared.copyColorShader)
        || !context.resources.Get(shared.postprocPSO)) return false;
    if (!context.resources.Get(state.output) || state.width != context.width || state.height != context.height) {
        context.resources.Release(state.output);
        state.output = context.resources.CreateComputeTexture(context.width, context.height);
        state.width = context.width;
        state.height = context.height;
    }
    if (!context.resources.Get(state.constants))
        state.constants = context.resources.CreateConstantBuffer(sizeof(RayDebugConstants));
    if (!context.resources.Get(state.output) || !context.resources.Get(state.constants)
        || context.resources.Get(state.output)->GetBindlessIndex() == INVALID_BINDLESS_INDEX
        || context.resources.Get(state.output)->GetBindlessUavIndex() == INVALID_BINDLESS_INDEX) return false;
    state.scene = BuildRayScene(*context.renderScene, {context.cullingMask});
    state.gpu = shared.rayGeometry.Prepare(state.scene, context);
    return state.gpu.ready;
}

bool PrepareRayReflectionView(RenderPassContext& context, RenderViewResources& view,
    RenderSharedResources& shared, const OpaqueRenderPlan& opaquePlan)
{
    auto& state = view.rayReflection;
    state.scene = {};
    state.gpu = {};
    state.sceneLighting = false;
    state.diffuseIndirectEnabled = false;
    state.cameraOriginProvenAir = false;
    state.resolutionDivisor = 1;
    state.traceWidth = context.width;
    state.traceHeight = context.height;
    view.rayReflectionCovered = false;
    context.rayReflectionPassActive = false;
    context.rayReflectionReconstructionPrepared = false;
    state.reconstruction.prepared = state.reconstruction.rawSucceeded = state.reconstruction.temporalSucceeded = false;
    context.handles.rayReflectionResult = {};
    const auto reject = [&]() {
        state.reconstruction.historyValid = false;
        state.cameraOriginProvenAir = false;
        return false;
    };
    const auto& request = context.settings.modeRequest;
    const auto capabilities = context.renderer.GetCapabilities();
    if (!context.experimentalRayTracingEnabled || request.mode != RenderMode::HYBRID || !request.rayReflection
        || !capabilities.inlineRayQuery || !capabilities.bindless || !context.renderScene
        || !context.width || !context.height) return reject();
    RayDebugConstants camera{};
    if (!MakeRayDebugCameraConstants(context.camera, camera)) return reject();
    state.scene = BuildRayScene(*context.renderScene, {context.cullingMask, true});
    view.rayReflectionCovered = state.scene.diagnostics.empty() && state.scene.surfaceDiagnostics.empty()
        && opaquePlan.UsesDeferredLighting() && !context.settings.IsUnlit() && !context.settings.IsWireframe()
        && !IsRayDebugView(context.settings.viewMode) && context.rayPathLightingSupported
        && context.rayLightsComplete && context.cloudShadowStrength == 0.0f
        && !context.settings.postProcess.fog.enabled && !context.settings.froxelFog.enabled
        && !context.settings.volumetricLight.enabled && !context.environment.cameraUnderwater
        && !context.environment.underwater.enabled && !context.environment.cloudEnabled;
    for (const auto& override : context.settings.passOverrides)
        if (!override.enabled && (override.name == "RayReflection" || override.name == "DeferredGBuffer"))
            view.rayReflectionCovered = false;
    bool hasGlass = false;
    for (const auto& instance : state.scene.instances) {
        hasGlass |= instance.surface.dielectric.transmission != 0.0f;
        if (instance.surface.dielectric.transmission != 0.0f
            && !IsHybridRayDielectricSupported(instance.surface)) {
            state.scene.surfaceDiagnostics.push_back({instance.objectId, instance.sourceItem,
                SurfaceMaterialIssue::SOLID_DIELECTRIC_UNSUPPORTED});
            view.rayReflectionCovered = false;
        }
        const auto& key = state.scene.geometries[instance.geometryIndex].key;
        const auto& item = context.renderScene->items[instance.sourceItem];
        if (key.vertexStride != sizeof(Vertex) || key.positionOffset != offsetof(Vertex, position)
            || context.renderScene->objects[item.objectIndex].rayLodSelectionRequired)
            view.rayReflectionCovered = false;
    }
    /// @note Glass-free scenes retain authored instance culling and their existing unsupported-backface fallback.
    if (hasGlass && view.rayReflectionCovered)
        state.scene = BuildRayScene(*context.renderScene, {context.cullingMask, true, true});
    RayPathLighting lighting;
    lighting.useSceneLights = true;
    lighting.lights = context.rayLights;
    lighting.supported = view.rayReflectionCovered;
    const auto& environment = context.environment.rayEnvironment;
    state.constantEnvironmentRadiance = {};
    state.constantEnvironmentKnown = ResolveRayConstantEnvironment(context.environment, context.camera,
        state.constantEnvironmentRadiance);
    if (state.constantEnvironmentKnown) lighting.environmentRadiance = state.constantEnvironmentRadiance;
    if (environment.requested) {
        lighting.supported = lighting.supported && environment.ready && environment.pixels
            && context.resources.Get(environment.rawTexture) && std::isfinite(environment.intensity)
            && environment.intensity >= 0 && std::isfinite(environment.rotationRadians);
        if (environment.pixels) {
            lighting.environmentContentVersion = environment.pixels->contentVersion;
            lighting.environmentRotation = environment.rotationRadians;
            lighting.environmentIntensity = environment.intensity;
        }
    }
    state.pathScene = state.sceneBuilder.Build(state.scene, context.resources, lighting, true);
    view.rayReflectionCovered = view.rayReflectionCovered && state.pathScene.coverageComplete;
    if (!view.rayReflectionCovered) return reject();
    state.gpu = shared.rayGeometry.Prepare(state.scene, context);
    if (!state.gpu.ready) return reject();
    state.provenAirOrigin = {camera.cameraPosition.x, camera.cameraPosition.y, camera.cameraPosition.z};
    state.cameraOriginProvenAir = hasGlass && !camera.orthographic
        && state.mediumCache.IsOriginProvenAir(state.scene, context.resources, state.provenAirOrigin);
    auto& resources = context.resources;
    if (state.pathScene.emitters.size() > UINT32_MAX || state.pathScene.deltaLights.size() > UINT32_MAX
        || state.pathScene.shapes.size() > UINT32_MAX) return reject();
    if (state.emitterRevision != state.pathScene.contentRevision
        || (!state.pathScene.emitters.empty() && !resources.Get(state.emitters))
        || (!state.pathScene.deltaLights.empty() && !resources.Get(state.deltaLights))
        || (!state.pathScene.shapes.empty() && !resources.Get(state.shapes))) {
        resources.Release(state.emitters); resources.Release(state.deltaLights); resources.Release(state.shapes);
        state.emitters = {}; state.deltaLights = {}; state.shapes = {};
        if (!state.pathScene.emitters.empty()) state.emitters = resources.CreateStructuredBuffer(state.pathScene.emitters.data(),
            static_cast<uint32_t>(state.pathScene.emitters.size()), sizeof(RayPathEmitterRecord));
        if (!state.pathScene.deltaLights.empty()) state.deltaLights = resources.CreateStructuredBuffer(state.pathScene.deltaLights.data(),
            static_cast<uint32_t>(state.pathScene.deltaLights.size()), sizeof(RayPathDeltaRecord));
        if (!state.pathScene.shapes.empty()) state.shapes = resources.CreateStructuredBuffer(state.pathScene.shapes.data(),
            static_cast<uint32_t>(state.pathScene.shapes.size()), sizeof(RayPathShapeRecord));
        state.emitterRevision = state.pathScene.contentRevision;
    }
    if (environment.requested) {
        if (state.environmentDistribution.contentVersion != environment.pixels->contentVersion
            || !resources.Get(state.environmentTable)) {
            RayEnvironmentDistribution distribution;
            if (!BuildRayEnvironmentDistribution(environment, distribution)) return reject();
            const auto table = resources.CreateStructuredBuffer(distribution.records.data(),
                static_cast<uint32_t>(distribution.records.size()), sizeof(RayEnvironmentRecord));
            if (!resources.Get(table)) return reject();
            resources.Release(state.environmentTable);
            state.environmentTable = table;
            state.environmentDistribution = std::move(distribution);
        }
        state.environment = environment.rawTexture;
    } else {
        resources.Release(state.environmentTable);
        state.environmentTable = {}; state.environment = {}; state.environmentDistribution = {};
    }
    const auto tableReady = [&](ResourceHandle<StructuredBufferTag> table, bool required) {
        return !required || (resources.Get(table) && resources.Get(table)->GetBindlessIndex() != INVALID_BINDLESS_INDEX);
    };
    if (!tableReady(state.emitters, !state.pathScene.emitters.empty())
        || !tableReady(state.deltaLights, !state.pathScene.deltaLights.empty())
        || !tableReady(state.shapes, !state.pathScene.shapes.empty())
        || !tableReady(state.environmentTable, environment.requested)
        || (environment.requested && resources.Get(state.environment)->GetBindlessIndex() == INVALID_BINDLESS_INDEX)) return reject();
    state.sceneLighting = true;
    state.diffuseIndirectEnabled = IsRayDiffuseIndirectReady(context, view);
    if (!context.resources.Get(shared.rayReflectionShader))
        shared.rayReflectionShader = context.resources.LoadShader("Assets/Shaders/RayTracing/RayReflection.cs.hlsl");
    if (!context.resources.Get(state.output) || state.width != context.width || state.height != context.height) {
        context.resources.Release(state.output);
        state.output = context.resources.CreateComputeTexture(context.width, context.height);
        state.width = context.width;
        state.height = context.height;
    }
    if (!context.resources.Get(state.constants))
        state.constants = context.resources.CreateConstantBuffer(sizeof(RayReflectionConstants));
    const auto* output = context.resources.Get(state.output);
    if (!context.resources.Get(shared.rayReflectionShader) || !context.resources.Get(state.constants)
        || !output || output->GetBindlessIndex() == INVALID_BINDLESS_INDEX
        || output->GetBindlessUavIndex() == INVALID_BINDLESS_INDEX) return reject();
    context.handles.rayReflectionResult = state.output;
    context.rayReflectionPassActive = true;
    const auto& quality = context.settings.hybridQuality;
    if (IsHybridQualityValid(quality) && quality.reflectionResolutionDivisor == 2
        && (context.width > 1 || context.height > 1)) {
        state.resolutionDivisor = 2;
        state.traceWidth = context.width / 2 + context.width % 2;
        state.traceHeight = context.height / 2 + context.height % 2;
    }
    const auto restoreFullResolution = [&]() {
        resources.Release(state.halfRaw);
        state.halfRaw = {};
        state.resolutionDivisor = 1;
        state.traceWidth = context.width;
        state.traceHeight = context.height;
        state.reconstruction.historyValid = false;
    };
    if (state.resolutionDivisor == 1) {
        resources.Release(state.halfRaw);
        state.halfRaw = {};
    }
    if (!PrepareRayReflectionReconstruction(context, view, shared)) restoreFullResolution();
    else if (state.resolutionDivisor == 2) {
        const auto* current = resources.Get(state.halfRaw);
        if (!current || current->GetWidth() != state.traceWidth || current->GetHeight() != state.traceHeight) {
            resources.Release(state.halfRaw);
            state.halfRaw = resources.CreateComputeTexture(state.traceWidth, state.traceHeight);
        }
        const auto* halfRaw = resources.Get(state.halfRaw);
        if (!halfRaw || halfRaw->GetWidth() != state.traceWidth || halfRaw->GetHeight() != state.traceHeight
            || halfRaw->GetBindlessIndex() == INVALID_BINDLESS_INDEX
            || halfRaw->GetBindlessUavIndex() == INVALID_BINDLESS_INDEX) {
            /// @note A failed mixed allocation restores full RAW transport and only keeps reconstruction if its smaller working set is ready.
            restoreFullResolution();
            (void)PrepareRayReflectionReconstruction(context, view, shared);
        }
    }
    return true;
}

void BuildRayReflectionPipeline(RenderPipeline& pipeline, RenderViewResources& view,
    RenderSharedResources& shared, RenderPassContext& context)
{
    auto& state = view.rayReflection;
    RenderGraph::ResourceDesc texture;
    texture.kind = RenderGraph::ResourceKind::Texture;
    texture.width = state.width;
    texture.height = state.height;
    texture.format = Format::RGBA16F;
    texture.transient = false;
    texture.allowAliasing = false;
    auto& reconstruction = state.reconstruction;
    pipeline.DeclareTexture("RayReflectionResult", reconstruction.prepared ? reconstruction.output : state.output, texture);
    if (reconstruction.prepared) {
        pipeline.DeclareTexture("RayReflectionRaw", state.output, texture);
        if (state.resolutionDivisor == 2) {
            auto halfTexture = texture;
            halfTexture.width = state.traceWidth;
            halfTexture.height = state.traceHeight;
            pipeline.DeclareTexture("RayReflectionHalfRaw", state.halfRaw, halfTexture);
        }
        RenderGraph::ResourceDesc buffer;
        buffer.external = true; buffer.transient = false; buffer.allowAliasing = false;
        const uint64_t count = static_cast<uint64_t>(state.width) * state.height;
        buffer.stride = sizeof(RayReflectionSurface); buffer.byteSize = count * buffer.stride;
        pipeline.DeclareStructuredBuffer("RayReflectionSurface", reconstruction.surfaces[reconstruction.surfaceReadIndex ^ 1u], buffer);
        pipeline.DeclareStructuredBuffer("RayReflectionSurfacePrevious", reconstruction.surfaces[reconstruction.surfaceReadIndex], buffer);
        buffer.stride = sizeof(RayReflectionHistoryRecord); buffer.byteSize = count * buffer.stride;
        pipeline.DeclareStructuredBuffer("RayReflectionHistory", reconstruction.histories[reconstruction.surfaceReadIndex ^ 1u], buffer);
        pipeline.DeclareStructuredBuffer("RayReflectionHistoryPrevious", reconstruction.histories[reconstruction.surfaceReadIndex], buffer);
    }
    RenderGraph::ResourceDesc imported;
    imported.external = true;
    imported.transient = false;
    imported.allowAliasing = false;
    const auto declareLightingTexture = [&](std::string_view name, ResourceHandle<TextureTag> handle) {
        auto description = imported;
        if (const auto* source = context.resources.Get(handle)) {
            description.width = source->GetWidth();
            description.height = source->GetHeight();
        }
        pipeline.DeclareTexture(std::string(name), handle, description);
    };
    declareLightingTexture("RayReflectionIrradiance", context.handles.iblIrradiance);
    declareLightingTexture("RayReflectionPrefilter", context.handles.iblPrefilter);
    declareLightingTexture("RayReflectionBrdfLut", context.handles.iblBrdfLut);
    for (size_t slot = 0; slot < 2; ++slot)
        declareLightingTexture("RayReflectionLightProbe" + std::to_string(slot),
            state.diffuseIndirectEnabled && view.advancedGraphicsSnapshot.probeVolumes[slot].intensity > 0
                ? context.handles.lightProbeSH[slot] : ResourceHandle<TextureTag>{});
    imported.byteSize = state.pathScene.emitters.size() * sizeof(RayPathEmitterRecord);
    imported.stride = sizeof(RayPathEmitterRecord);
    pipeline.DeclareStructuredBuffer("RayReflectionEmitters", state.emitters, imported);
    imported.byteSize = state.pathScene.deltaLights.size() * sizeof(RayPathDeltaRecord);
    imported.stride = sizeof(RayPathDeltaRecord);
    pipeline.DeclareStructuredBuffer("RayReflectionDeltaLights", state.deltaLights, imported);
    imported.byteSize = state.pathScene.shapes.size() * sizeof(RayPathShapeRecord);
    imported.stride = sizeof(RayPathShapeRecord);
    pipeline.DeclareStructuredBuffer("RayReflectionShapes", state.shapes, imported);
    imported.byteSize = state.environmentDistribution.records.size() * sizeof(RayEnvironmentRecord);
    imported.stride = sizeof(RayEnvironmentRecord);
    pipeline.DeclareStructuredBuffer("RayReflectionEnvironmentTable", state.environmentTable, imported);
    RenderGraph::ResourceDesc externalTexture;
    externalTexture.external = true;
    externalTexture.transient = false;
    externalTexture.allowAliasing = false;
    if (const auto* raw = context.resources.Get(state.environment)) {
        externalTexture.width = raw->GetWidth();
        externalTexture.height = raw->GetHeight();
    }
    pipeline.DeclareTexture("RayReflectionEnvironment", state.environment, externalTexture);
    if (state.gpu.instanceCount) {
        pipeline.DeclareAccelerationStructure("RaySceneTLAS", state.gpu.topLevel, imported);
        imported.byteSize = static_cast<uint64_t>(state.gpu.instanceCount) * sizeof(RayHitRecord);
        imported.stride = sizeof(RayHitRecord);
        pipeline.DeclareStructuredBuffer("RayHitRecords", state.gpu.hitRecords, imported);
        imported.byteSize = static_cast<uint64_t>(state.gpu.instanceCount) * sizeof(RaySurfaceRecord);
        imported.stride = sizeof(RaySurfaceRecord);
        pipeline.DeclareStructuredBuffer("RaySurfaceMaterials", state.gpu.surfaceMaterials, imported);
        for (size_t i = 0; i < state.gpu.readBuffers.size(); ++i) {
            const auto* buffer = context.resources.Get(state.gpu.readBuffers[i]);
            imported.byteSize = buffer ? buffer->GetSize() : 0;
            imported.stride = buffer ? buffer->GetStride() : 0;
            pipeline.DeclareBuffer("RayReadBuffer" + std::to_string(i), state.gpu.readBuffers[i], imported);
        }
        for (size_t i = 0; i < state.gpu.readTextures.size(); ++i) {
            RenderGraph::ResourceDesc materialTexture = externalTexture;
            materialTexture.width = materialTexture.height = 0;
            materialTexture.format = Format::RGBA8;
            if (const auto* source = context.resources.Get(state.gpu.readTextures[i])) {
                materialTexture.width = source->GetWidth();
                materialTexture.height = source->GetHeight();
            }
            pipeline.DeclareTexture("RayReadTexture" + std::to_string(i), state.gpu.readTextures[i], materialTexture);
        }
    }
    RayReflectionLightingResources lighting;
    lighting.emitters = state.emitters; lighting.deltaLights = state.deltaLights; lighting.shapes = state.shapes;
    lighting.environment = state.environment; lighting.environmentTable = state.environmentTable;
    lighting.sceneLighting = state.sceneLighting;
    lighting.diffuseIndirectEnabled = state.diffuseIndirectEnabled;
    lighting.glassEnabled = std::any_of(state.scene.instances.begin(), state.scene.instances.end(),
        [](const RaySceneInstance& instance) { return IsHybridRayDielectricSupported(instance.surface); });
    lighting.cameraOriginProvenAir = state.cameraOriginProvenAir;
    lighting.provenAirOrigin = state.provenAirOrigin;
    lighting.constantEnvironmentKnown = state.constantEnvironmentKnown;
    lighting.constantEnvironmentRadiance = state.constantEnvironmentRadiance;
    lighting.emitterCount = static_cast<uint32_t>(state.pathScene.emitters.size());
    lighting.deltaLightCount = static_cast<uint32_t>(state.pathScene.deltaLights.size());
    lighting.shapeCount = static_cast<uint32_t>(state.pathScene.shapes.size());
    lighting.environmentTableCount = static_cast<uint32_t>(state.environmentDistribution.records.size());
    lighting.environmentFaceSize = state.environmentDistribution.faceSize;
    lighting.environmentIntensity = context.environment.rayEnvironment.intensity;
    lighting.environmentRotation = context.environment.rayEnvironment.rotationRadians;
    pipeline.AddPass<RayReflectionPass>(state.gpu, state.constants, shared.rayReflectionShader, false, lighting,
        reconstruction.prepared ? &reconstruction : nullptr, state.halfRaw);
    if (reconstruction.prepared) {
        pipeline.AddPass<RayReflectionReconstructionPass>(state, shared.rayReflectionReconstructionShader, false);
        pipeline.AddPass<RayReflectionReconstructionPass>(state, shared.rayReflectionReconstructionShader, true);
    }
}

void BuildRayDebugPipeline(RenderPipeline& pipeline, RenderViewResources& view,
    RenderSharedResources& shared, ResourceManager& resources, std::string_view outputName)
{
    auto& state = view.rayDebug;
    using Kind = RenderGraph::ResourceKind;
    RenderGraph::ResourceDesc texture;
    texture.kind = Kind::Texture;
    texture.width = state.width;
    texture.height = state.height;
    texture.external = false;
    texture.transient = false;
    texture.allowAliasing = false;
    pipeline.DeclareTexture("RayDebugResult", state.output, texture);
    if (state.gpu.instanceCount) {
        RenderGraph::ResourceDesc imported;
        imported.external = true;
        imported.transient = false;
        imported.allowAliasing = false;
        pipeline.DeclareAccelerationStructure("RaySceneTLAS", state.gpu.topLevel, imported);
        imported.byteSize = static_cast<uint64_t>(state.gpu.instanceCount) * sizeof(RayHitRecord);
        imported.stride = sizeof(RayHitRecord);
        pipeline.DeclareStructuredBuffer("RayHitRecords", state.gpu.hitRecords, imported);
        for (size_t i = 0; i < state.gpu.readBuffers.size(); ++i) {
            /// @note 型付きハンドルを個別に申告し、table 背後の Buffer を backend へ渡す。
            const auto* buffer = resources.Get(state.gpu.readBuffers[i]);
            imported.byteSize = buffer ? buffer->GetSize() : 0;
            imported.stride = buffer ? buffer->GetStride() : 0;
            pipeline.DeclareBuffer("RayReadBuffer" + std::to_string(i), state.gpu.readBuffers[i], imported);
        }
    }
    pipeline.AddPass<RayDebugPass>(state.gpu, state.constants, shared.rayDebugShader, !state.scene.diagnostics.empty());
    pipeline.AddPass<RayDebugCopyPass>(std::string(outputName), shared.copyColorShader, shared.postprocPSO);
}
} /// @note namespace fbzz::renderer
