/// @file    PathTracingPipeline.cpp
/// @brief   Reference Path の被覆判定・Progressive 資源準備・専用ビュー構成。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#include <Graphics/Pipeline/ViewPipeline.hpp>
#include <Graphics/Passes/RayTracing/RayPathTracePass.hpp>
#include <Graphics/Passes/RayTracing/RayGameReconstructionPass.hpp>
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Passes/PostProcess/PostProcessPasses.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Math/MathUtils.hpp>
#include <algorithm>
#include <bit>
#include <limits>

namespace fbzz::renderer {
namespace {

RayPathLighting MakePathLighting(const RenderPassContext& context)
{
    RayPathLighting result;
    const auto& light = context.lightData;
    result.direction = light.lightDir;
    /// @note 既存ライトは Lambert の 1/pi を省いた強度なので、放射輝度契約へ一度だけ変換する。
    result.directionalRadiance = light.lightColor * (light.lightIntensity * math::PI);
    result.environmentRadiance = {context.camera.m_backgroundColor.x,
        context.camera.m_backgroundColor.y, context.camera.m_backgroundColor.z};
    result.useSceneLights = context.rayLightsComplete;
    result.lights = context.rayLights;
    if (result.useSceneLights) {
        result.direction = {0, -1, 0};
        result.directionalRadiance = {};
    }
    const auto& environment = context.environment;
    const auto& raw = environment.rayEnvironment;
    if (raw.requested && raw.pixels) {
        result.environmentContentVersion = raw.pixels->contentVersion;
        result.environmentRotation = raw.rotationRadians;
        result.environmentIntensity = raw.intensity;
    }
    const bool blackSky = environment.sky && environment.sky->skyScatterIntensity == 0.0f;
    if (blackSky) result.environmentRadiance = {};
    result.supported = context.rayPathLightingSupported
        && (result.useSceneLights || (context.punctualLights.empty()
            && light.pointLightCount == 0 && light.spotLightCount == 0 && context.legacyShapedLightCount == 0))
        && (result.useSceneLights || context.lightCookieViewCount == 0) && context.cloudShadowStrength == 0.0f
        && (!environment.sky || blackSky) && !environment.cloudEnabled
        && (!environment.sunMoon || ((!environment.sunMoon->sunEnabled || environment.sunMoon->sunDiskIntensity == 0.0f)
            && (!environment.sunMoon->moonEnabled || environment.sunMoon->moonBrightness == 0.0f)))
        && !environment.cameraUnderwater && !environment.underwater.enabled && !environment.caustics.enabled
        && (!context.settings.ibl.enabled || raw.requested) && (!raw.requested || raw.ready)
        && !context.settings.postProcess.fog.enabled
        && !context.settings.froxelFog.enabled && !context.settings.volumetricLight.enabled;
    return result;
}

bool IsPathTextureReady(ResourceManager& resources, ResourceHandle<TextureTag> handle)
{
    const auto* texture = resources.Get(handle);
    return texture && texture->GetBindlessIndex() != INVALID_BINDLESS_INDEX
        && texture->GetBindlessUavIndex() != INVALID_BINDLESS_INDEX;
}

template<class T>
void AppendGameWords(std::vector<uint32_t>& key, const T& value)
{
    const auto words = std::bit_cast<std::array<uint32_t, sizeof(T) / sizeof(uint32_t)>>(value);
    key.insert(key.end(), words.begin(), words.end());
}

bool PrepareGameResources(RenderPassContext& context, RenderViewResources& view, RenderSharedResources& shared)
{
    auto& state = view.rayPath;
    auto& game = state.game;
    game.prepared = false;
    auto& resources = context.resources;
    const uint32_t count = context.width * context.height;
    if (!resources.Get(shared.rayGameReconstructionShader))
        shared.rayGameReconstructionShader = resources.LoadShader("Assets/Shaders/RayTracing/RayGameReconstruction.cs.hlsl");
    if (!resources.Get(shared.rayGameReconstructionShader) || !resources.Get(view.gbuffer)
        || !resources.Get(shared.gbufferShader)) return false;
    if (!resources.Get(game.transport) || !resources.Get(game.surface)
        || !resources.Get(game.reconstructionHistory[0]) || !resources.Get(game.reconstructionHistory[1])) {
        resources.Release(game.transport); resources.Release(game.surface);
        for (const auto history : game.reconstructionHistory) resources.Release(history);
        game.transport = resources.CreateRWStructuredBuffer(nullptr, count, sizeof(RayGameTransportRecord));
        game.surface = resources.CreateRWStructuredBuffer(nullptr, count, sizeof(RayReconstructionSurface));
        for (auto& history : game.reconstructionHistory)
            history = resources.CreateRWStructuredBuffer(nullptr, count, sizeof(RayReconstructionHistoryRecord));
        game.historyValid = false;
        game.historyReadIndex = 0;
    }
    if (!resources.Get(game.traceConstants)) game.traceConstants = resources.CreateConstantBuffer(sizeof(RayGameTraceConstants));
    if (!resources.Get(game.reconstructionConstants))
        game.reconstructionConstants = resources.CreateConstantBuffer(sizeof(RayReconstructionConstants));
    std::vector<RayGameMotionRecord> motion;
    std::vector<uint32_t> content;
    AppendGameWords(content, state.scene.sceneGeneration);
    AppendGameWords(content, resources.GetShaderVersion());
    AppendGameWords(content, state.pathScene.lighting.environmentContentVersion);
    /// @note カメラ姿勢・サンプル番号・reset は内容版でない。通常の移動は再投影へ渡す。
    AppendGameWords(content, state.constantsData.lightDirection);
    AppendGameWords(content, state.constantsData.lightRadiance);
    AppendGameWords(content, state.constantsData.environmentRadiance);
    AppendGameWords(content, state.constantsData.environmentMode);
    AppendGameWords(content, state.constantsData.environmentRotation);
    AppendGameWords(content, state.constantsData.environmentIntensity);
    AppendGameWords(content, state.constantsData.maxBounces);
    AppendGameWords(content, state.constantsData.rouletteStart);
    AppendGameWords(content, state.constantsData.samplerSeed);
    AppendGameWords(content, static_cast<uint32_t>(state.pathScene.emitters.size()));
    AppendGameWords(content, static_cast<uint32_t>(state.pathScene.deltaLights.size()));
    AppendGameWords(content, static_cast<uint32_t>(state.scene.instances.size()));
    for (const auto& emitter : state.pathScene.emitters) AppendGameWords(content, emitter);
    for (const auto& light : state.pathScene.deltaLights) AppendGameWords(content, light);
    for (const auto& instance : state.scene.instances) {
        const auto& item = context.renderScene->items[instance.sourceItem];
        const auto& object = context.renderScene->objects[item.objectIndex];
        const auto& geometry = state.scene.geometries[instance.geometryIndex].key;
        AppendGameWords(content, instance.objectId.sceneGeneration);
        AppendGameWords(content, instance.objectId.index); AppendGameWords(content, instance.objectId.generation);
        AppendGameWords(content, geometry.vertices.id); AppendGameWords(content, geometry.vertices.gen);
        AppendGameWords(content, geometry.indices.id); AppendGameWords(content, geometry.indices.gen);
        if (!object.skinned) AppendGameWords(content, geometry.vertexContentVersion);
        AppendGameWords(content, geometry.indexContentVersion);
        AppendGameWords(content, geometry.vertexCount); AppendGameWords(content, geometry.indexCount);
        AppendGameWords(content, geometry.vertexStride); AppendGameWords(content, geometry.positionOffset);
        AppendGameWords(content, geometry.firstVertex); AppendGameWords(content, geometry.firstIndex);
        AppendGameWords(content, instance.sourceSubmesh); AppendGameWords(content, instance.materialSlot);
        AppendGameWords(content, MakeRaySurfaceRecord(instance.surface));
        for (const auto& binding : instance.surface.textures) {
            AppendGameWords(content, binding.texture.id); AppendGameWords(content, binding.texture.gen);
            AppendGameWords(content, binding.contentVersion);
        }
        RayGameMotionRecord record;
        record.currentWorldInverse = math::Matrix4::Inverse(object.world);
        record.previousWorld = instance.previousWorld;
        /// @note previous palette は previous deformed positions の保証ではない。Skinned は時間履歴を拒否する。
        record.temporalValid = !object.skinned && game.historyValid && context.frameStamp == game.lastFrameStamp + 1;
        motion.push_back(record);
    }
    const bool reset = !game.historyValid || content != game.contentKey
        || IsRayGameCameraCut(game.previousCamera, context.camera)
        || (game.historyValid && context.frameStamp != game.lastFrameStamp + 1);
    game.contentKey = std::move(content);
    game.reconstructed = false;
    game.temporalSucceeded = false;
    game.traceData.frameSampleIndex = game.frameSampleIndex;
    game.reconstructionData = {};
    RayPathTraceConstants previous;
    if (!MakeRayPathCameraConstants(reset ? context.camera : game.previousCamera, previous)) return false;
    game.reconstructionData.previousCameraPosition = previous.cameraPosition;
    game.reconstructionData.previousCameraRight = previous.cameraRight;
    game.reconstructionData.previousCameraUp = previous.cameraUp;
    game.reconstructionData.previousCameraForward = previous.cameraForward;
    game.reconstructionData.previousOrthographic = previous.orthographic;
    game.reconstructionData.width = context.width; game.reconstructionData.height = context.height;
    game.reconstructionData.resetHistory = reset;
    resources.Release(game.motionInstances);
    game.motionInstances = {};
    /// @note 毎フレームの変換表は immutable UPLOAD。default buffer 作成時の同期 Flush を避ける。
    if (!motion.empty()) game.motionInstances = resources.CreateStructuredBuffer(motion.data(),
        static_cast<uint32_t>(motion.size()), sizeof(RayGameMotionRecord));
    /// @note Lazy descriptor allocation must succeed before the view can claim Game availability.
    const auto readWriteReady = [&](ResourceHandle<StructuredBufferTag> handle) {
        const auto* buffer = resources.Get(handle);
        return buffer && buffer->GetBindlessIndex() != INVALID_BINDLESS_INDEX
            && buffer->GetBindlessUavIndex() != INVALID_BINDLESS_INDEX;
    };
    const auto* motionBuffer = resources.Get(game.motionInstances);
    game.prepared = readWriteReady(game.transport) && readWriteReady(game.surface)
        && readWriteReady(game.reconstructionHistory[0]) && readWriteReady(game.reconstructionHistory[1])
        && resources.Get(game.traceConstants) && resources.Get(game.reconstructionConstants)
        && (motion.empty() || (motionBuffer && motionBuffer->GetBindlessIndex() != INVALID_BINDLESS_INDEX));
    return game.prepared;
}

} /// @note namespace

bool PrepareRayPathView(RenderPassContext& context, RenderViewResources& view, RenderSharedResources& shared)
{
    auto& state = view.rayPath;
    state.gpu = {};
    state.scene = {};
    state.dispatchSucceeded = false;
    view.rayPathCovered = false;
    view.rayPathPrepared = false;
    context.rayPathPassActive = false;
    const auto& request = context.settings.modeRequest;
    const auto capabilities = context.renderer.GetCapabilities();
    if (request.mode != RenderMode::PATH_TRACING
        || !capabilities.inlineRayQuery || !capabilities.bindless || !context.renderScene
        || !context.width || !context.height) return false;
    RayPathTraceConstants constants;
    state.gameProfile = request.pathProfile == PathTracingProfile::GAME;
    if (!MakeRayPathCameraConstants(context.camera, constants)) return false;
    const auto& environment = context.environment.rayEnvironment;
    if (environment.requested) {
        if (!environment.ready || !environment.pixels || !context.resources.Get(environment.rawTexture)) return false;
        if (state.environmentDistribution.contentVersion != environment.pixels->contentVersion
            || !context.resources.Get(state.environmentTable)) {
            RayEnvironmentDistribution distribution;
            if (!BuildRayEnvironmentDistribution(environment, distribution)) return false;
            const auto table = context.resources.CreateStructuredBuffer(distribution.records.data(),
                static_cast<uint32_t>(distribution.records.size()), sizeof(RayEnvironmentRecord));
            if (!context.resources.Get(table)) return false;
            context.resources.Release(state.environmentTable);
            state.environmentTable = table;
            state.environmentDistribution = std::move(distribution);
        }
        state.environment = environment.rawTexture;
    } else {
        context.resources.Release(state.environmentTable);
        state.environmentTable = {};
        state.environment = {};
        state.environmentDistribution = {};
    }
    state.scene = BuildRayScene(*context.renderScene, {context.cullingMask, true});
    state.pathScene = state.sceneBuilder.Build(state.scene, context.resources, MakePathLighting(context));
    if (state.pathScene.emitters.size() > UINT32_MAX || state.pathScene.deltaLights.size() > UINT32_MAX
        || state.pathScene.shapes.size() > UINT32_MAX) return false;
    view.rayPathCovered = state.pathScene.coverageComplete && !context.settings.IsUnlit()
        && !context.settings.IsWireframe() && !IsRayDebugView(context.settings.viewMode)
        && context.camera.m_clearMode == CameraClearMode::SolidColor;
    for (const auto& instance : state.scene.instances) {
        const auto& key = state.scene.geometries[instance.geometryIndex].key;
        const auto& item = context.renderScene->items[instance.sourceItem];
        const auto& object = context.renderScene->objects[item.objectIndex];
        if (key.vertexStride != sizeof(Vertex) || key.positionOffset != offsetof(Vertex, position)
            || instance.doubleSided || object.rayLodSelectionRequired)
            view.rayPathCovered = false;
        /// @note テクスチャなしの定数 alpha=1 は cutoff 未満でなければ全被覆。無作用の clip 閾値も Raster primary と一致する。
        if (state.gameProfile && (!instance.surface.standardSurfaceSupported || instance.surface.textureMask != 0
            || item.material.rayCapabilities.opacity != RayOpacity::OPAQUE_SURFACE
            || instance.surface.alphaCutoff > instance.surface.baseColor.w || instance.surface.baseColor.w != 1
            || !object.lodVisible || object.lodDither != 0
            || ResolveGeometryRoute(item.material.capabilities, true) != GeometryRoute::GBuffer)) view.rayPathCovered = false;
    }
    if (state.gameProfile) {
        /// @note Raster primary に存在しない analytic light は初期 Game の whole-view fallback。
        for (const auto& emitter : state.pathScene.emitters)
            if ((emitter.flags & RAY_PATH_EMITTER_VIRTUAL) != 0) view.rayPathCovered = false;
        if (!state.pathScene.shapes.empty()) view.rayPathCovered = false;
    }
    for (const auto& override : context.settings.passOverrides)
        if (!override.enabled && (override.name == "RayPathTrace" || override.name == "RayPathResolve"
            || override.name == "RayPathColor" || override.name == "Composite"
            || (state.gameProfile && (override.name == "DeferredGBuffer" || override.name == "RayGameTemporal"
                || override.name == "RayGameSpatial")))) view.rayPathCovered = false;
    if (!view.rayPathCovered) return false;
    state.gpu = shared.rayGeometry.Prepare(state.scene, context);
    if (!state.gpu.ready) return false;
    auto& resources = context.resources;
    if (!resources.Get(shared.rayPathShader))
        shared.rayPathShader = resources.LoadShader("Assets/Shaders/RayTracing/RayPathTrace.cs.hlsl");
    if (!resources.Get(shared.rayPathResolveShader))
        shared.rayPathResolveShader = resources.LoadShader("Assets/Shaders/RayTracing/RayPathResolve.hlsl");
    if (!resources.Get(shared.rayPathResolvePSO))
        shared.rayPathResolvePSO = resources.CreatePipelineState({RasterizerMode::SOLID_NOCULL,
            BlendMode::OPAQUE_BLEND, DepthMode::DEPTH_ON});
    if (!resources.Get(shared.rayPathShader) || !resources.Get(shared.rayPathResolveShader)
        || !resources.Get(shared.rayPathResolvePSO) || !resources.Get(shared.copyColorShader)
        || !resources.Get(shared.postprocPSO)) return false;
    const uint64_t pixelCount = static_cast<uint64_t>(context.width) * context.height;
    if (pixelCount > UINT32_MAX) return false;
    if (state.width != context.width || state.height != context.height || !resources.Get(state.output)
        || !resources.Get(state.firstSurface) || !resources.Get(state.firstMaterial)
        || !resources.Get(state.firstGeometry) || !resources.Get(state.historyBuffer) || !resources.Get(state.idsBuffer)) {
        resources.Release(state.output);
        resources.Release(state.firstSurface);
        resources.Release(state.firstMaterial);
        resources.Release(state.firstGeometry);
        resources.Release(state.historyBuffer);
        resources.Release(state.idsBuffer);
        resources.Release(state.game.transport); resources.Release(state.game.surface);
        for (const auto history : state.game.reconstructionHistory) resources.Release(history);
        resources.Release(state.game.motionInstances); resources.Release(state.game.traceConstants);
        resources.Release(state.game.reconstructionConstants);
        state.game = {};
        state.output = resources.CreateComputeTexture(context.width, context.height);
        state.firstSurface = resources.CreateComputeTexture(context.width, context.height);
        state.firstMaterial = resources.CreateComputeTexture(context.width, context.height);
        state.firstGeometry = resources.CreateComputeTexture(context.width, context.height);
        /// @note 初期値は shader が reset 時に全画素へ書く。nullptr によりフレーム内の同期 upload を避ける。
        state.historyBuffer = resources.CreateRWStructuredBuffer(nullptr, static_cast<uint32_t>(pixelCount), sizeof(RayPathHistoryRecord));
        state.idsBuffer = resources.CreateRWStructuredBuffer(nullptr, static_cast<uint32_t>(pixelCount), sizeof(RayPathIdRecord));
        state.width = context.width;
        state.height = context.height;
        state.history.Reset();
    }
    if (!resources.Get(state.constants))
        state.constants = resources.CreateConstantBuffer(sizeof(RayPathTraceConstants));
    if (state.emitterRevision != state.pathScene.contentRevision
        || (!state.pathScene.emitters.empty() && !resources.Get(state.emitters))
        || (!state.pathScene.deltaLights.empty() && !resources.Get(state.deltaLights))
        || (!state.pathScene.shapes.empty() && !resources.Get(state.shapes))) {
        resources.Release(state.emitters);
        resources.Release(state.deltaLights);
        resources.Release(state.shapes);
        state.emitters = {};
        state.deltaLights = {};
        state.shapes = {};
        if (!state.pathScene.emitters.empty())
            state.emitters = resources.CreateStructuredBuffer(state.pathScene.emitters.data(),
                static_cast<uint32_t>(state.pathScene.emitters.size()), sizeof(RayPathEmitterRecord));
        if (!state.pathScene.deltaLights.empty())
            state.deltaLights = resources.CreateStructuredBuffer(state.pathScene.deltaLights.data(),
                static_cast<uint32_t>(state.pathScene.deltaLights.size()), sizeof(RayPathDeltaRecord));
        if (!state.pathScene.shapes.empty())
            state.shapes = resources.CreateStructuredBuffer(state.pathScene.shapes.data(),
                static_cast<uint32_t>(state.pathScene.shapes.size()), sizeof(RayPathShapeRecord));
        state.emitterRevision = state.pathScene.contentRevision;
    }
    const auto* historyBuffer = resources.Get(state.historyBuffer);
    const auto* idsBuffer = resources.Get(state.idsBuffer);
    if (!IsPathTextureReady(resources, state.output) || !IsPathTextureReady(resources, state.firstSurface)
        || !IsPathTextureReady(resources, state.firstMaterial) || !IsPathTextureReady(resources, state.firstGeometry)
        || !resources.Get(state.constants) || !historyBuffer || !idsBuffer
        || historyBuffer->GetBindlessUavIndex() == INVALID_BINDLESS_INDEX
        || idsBuffer->GetBindlessUavIndex() == INVALID_BINDLESS_INDEX
        || (!state.pathScene.emitters.empty() && (!resources.Get(state.emitters)
            || resources.Get(state.emitters)->GetBindlessIndex() == INVALID_BINDLESS_INDEX))
        || (!state.pathScene.deltaLights.empty() && (!resources.Get(state.deltaLights)
            || resources.Get(state.deltaLights)->GetBindlessIndex() == INVALID_BINDLESS_INDEX))
        || (!state.pathScene.shapes.empty() && (!resources.Get(state.shapes)
            || resources.Get(state.shapes)->GetBindlessIndex() == INVALID_BINDLESS_INDEX))
        || (environment.requested && (resources.Get(state.environment)->GetBindlessIndex() == INVALID_BINDLESS_INDEX
            || resources.Get(state.environmentTable)->GetBindlessIndex() == INVALID_BINDLESS_INDEX))) return false;
    RayPathIntegratorSettings integrator;
    integrator.maxDistance = (std::numeric_limits<float>::max)();
    /// @note device 再生成は RenderResources ごと破棄する。shader hot reload は Manager の版で棄却する。
    const auto key = MakeRayPathHistoryKey(state.pathScene, context.camera, context.width, context.height,
        integrator, 1, resources.GetShaderVersion());
    constants.resetHistory = state.history.Prepare(key) || state.history.GetSampleCount() == 0 ? 1u : 0u;
    constants.width = context.width;
    constants.height = context.height;
    constants.instanceCount = state.gpu.instanceCount;
    constants.emitterCount = static_cast<uint32_t>(state.pathScene.emitters.size());
    constants.deltaLightCount = static_cast<uint32_t>(state.pathScene.deltaLights.size());
    constants.shapeLightCount = static_cast<uint32_t>(state.pathScene.shapes.size());
    constants.sampleBase = state.history.GetSampleCount();
    constants.maxBounces = integrator.maxBounces;
    constants.rouletteStart = integrator.rouletteStartBounce;
    constants.samplerSeed = integrator.seed;
    const auto& lighting = state.pathScene.lighting;
    constants.lightDirection = {lighting.direction.x, lighting.direction.y, lighting.direction.z, 0};
    constants.lightRadiance = {lighting.directionalRadiance.x, lighting.directionalRadiance.y, lighting.directionalRadiance.z, 0};
    constants.environmentRadiance = {lighting.environmentRadiance.x, lighting.environmentRadiance.y, lighting.environmentRadiance.z, 1};
    constants.environmentMode = environment.requested ? 2u : 1u;
    constants.environmentTableCount = static_cast<uint32_t>(state.environmentDistribution.records.size());
    constants.environmentFaceSize = state.environmentDistribution.faceSize;
    constants.environmentRotation = environment.rotationRadians;
    constants.environmentIntensity = environment.intensity;
    state.constantsData = constants;
    if (state.gameProfile && !PrepareGameResources(context, view, shared)) return false;
    context.rayPathPassActive = true;
    view.rayPathPrepared = true;
    return true;
}

void BuildRayPathViewPipeline(RenderPipeline& pipeline, RenderPassContext& context,
    RenderViewResources& view, RenderSharedResources& shared, const ViewPipelineExtensions& extensions)
{
    auto& state = view.rayPath;
    auto& resources = context.resources;
    using Kind = RenderGraph::ResourceKind;
    using Usage = RenderGraph::ResourceUsage;
    pipeline.SetPassOverrides(context.settings.passOverrides);
    pipeline.SetSchedulePolicy(context.settings.schedulePolicy);
    context.rayReflectionPassActive = false;
    context.rayPathViewActive = true;
    context.ssrPassActive = false;
    context.taaJitterNdcX = 0;
    context.taaJitterNdcY = 0;
    view.taaHistoryValid = false;
    view.taaFrameIndex = 0;
    if (extensions.begin) extensions.begin();
    const bool upscale = view.needsUpscale && view.upscaleSrc.IsValid();
    const char* chain = upscale ? "UpscaleSrc" : "Output";
    context.chainOutputRT = upscale ? view.upscaleSrc : context.outputRT;
    context.compositeOutputRT = context.chainOutputRT;
    pipeline.DeclareTarget("Output", context.outputRT,
        {Kind::RenderTarget, context.outputWidth, context.outputHeight, Format::RGBA16F, 1, true, true, false});
    if (upscale) pipeline.DeclareTarget("UpscaleSrc", view.upscaleSrc,
        {Kind::RenderTarget, context.width, context.height, Format::RGBA16F, 1, false, false, false});
    pipeline.DeclareTarget("HDR", view.hdr,
        {Kind::RenderTarget, context.width, context.height, Format::RGBA16F, 1, true, false, false});
    RenderGraph::ResourceDesc image;
    image.kind = Kind::Texture;
    image.width = context.width;
    image.height = context.height;
    image.transient = false;
    image.allowAliasing = false;
    pipeline.DeclareTexture("RayPathResult", state.output, image);
    pipeline.DeclareTexture("RayPathSurface", state.firstSurface, image);
    pipeline.DeclareTexture("RayPathMaterial", state.firstMaterial, image);
    pipeline.DeclareTexture("RayPathGeometry", state.firstGeometry, image);
    if (state.gameProfile) {
        pipeline.DeclareTarget("GBuffer", view.gbuffer,
            {Kind::RenderTarget, context.width, context.height, Format::RGBA16F, GBUFFER_COLOR_COUNT, true, false, false});
        pipeline.AddPass<GBufferPass>(GBufferPassMode::Deferred);
    }
    RenderGraph::ResourceDesc imported;
    imported.external = true;
    imported.transient = false;
    imported.allowAliasing = false;
    imported.byteSize = static_cast<uint64_t>(context.width) * context.height * sizeof(RayPathHistoryRecord);
    imported.stride = sizeof(RayPathHistoryRecord);
    pipeline.DeclareStructuredBuffer("RayPathHistory", state.historyBuffer, imported);
    imported.stride = sizeof(RayPathIdRecord);
    pipeline.DeclareStructuredBuffer("RayPathIds", state.idsBuffer, imported);
    imported.byteSize = state.pathScene.emitters.size() * sizeof(RayPathEmitterRecord);
    imported.stride = sizeof(RayPathEmitterRecord);
    pipeline.DeclareStructuredBuffer("RayPathEmitters", state.emitters, imported);
    imported.byteSize = state.pathScene.deltaLights.size() * sizeof(RayPathDeltaRecord);
    imported.stride = sizeof(RayPathDeltaRecord);
    pipeline.DeclareStructuredBuffer("RayPathDeltaLights", state.deltaLights, imported);
    imported.byteSize = state.pathScene.shapes.size() * sizeof(RayPathShapeRecord);
    imported.stride = sizeof(RayPathShapeRecord);
    pipeline.DeclareStructuredBuffer("RayPathShapes", state.shapes, imported);
    imported.byteSize = state.environmentDistribution.records.size() * sizeof(RayEnvironmentRecord);
    imported.stride = sizeof(RayEnvironmentRecord);
    pipeline.DeclareStructuredBuffer("RayPathEnvironmentTable", state.environmentTable, imported);
    auto environmentDesc = image;
    environmentDesc.external = true;
    const auto* environmentTexture = resources.Get(state.environment);
    environmentDesc.width = environmentTexture ? environmentTexture->GetWidth() : 0;
    environmentDesc.height = environmentTexture ? environmentTexture->GetHeight() : 0;
    pipeline.DeclareTexture("RayPathEnvironment", state.environment, environmentDesc);
    if (state.gameProfile) {
        imported.byteSize = static_cast<uint64_t>(context.width) * context.height * sizeof(RayGameTransportRecord);
        imported.stride = sizeof(RayGameTransportRecord);
        pipeline.DeclareStructuredBuffer("RayGameTransport", state.game.transport, imported);
        imported.byteSize = static_cast<uint64_t>(context.width) * context.height * sizeof(RayReconstructionSurface);
        imported.stride = sizeof(RayReconstructionSurface);
        pipeline.DeclareStructuredBuffer("RayGameSurface", state.game.surface, imported);
        imported.byteSize = static_cast<uint64_t>(context.width) * context.height * sizeof(RayReconstructionHistoryRecord);
        imported.stride = sizeof(RayReconstructionHistoryRecord);
        pipeline.DeclareStructuredBuffer("RayGameHistoryPrevious", state.game.reconstructionHistory[state.game.historyReadIndex], imported);
        pipeline.DeclareStructuredBuffer("RayGameHistoryNext", state.game.reconstructionHistory[state.game.historyReadIndex ^ 1u], imported);
        imported.byteSize = static_cast<uint64_t>(state.scene.instances.size()) * sizeof(RayGameMotionRecord);
        imported.stride = sizeof(RayGameMotionRecord);
        pipeline.DeclareStructuredBuffer("RayGameMotionInstances", state.game.motionInstances, imported);
    }
    if (state.gpu.instanceCount) {
        pipeline.DeclareAccelerationStructure("RaySceneTLAS", state.gpu.topLevel, imported);
        imported.byteSize = static_cast<uint64_t>(state.gpu.instanceCount) * sizeof(RayHitRecord);
        imported.stride = sizeof(RayHitRecord);
        pipeline.DeclareStructuredBuffer("RayHitRecords", state.gpu.hitRecords, imported);
        imported.byteSize = static_cast<uint64_t>(state.gpu.instanceCount) * sizeof(RaySurfaceRecord);
        imported.stride = sizeof(RaySurfaceRecord);
        pipeline.DeclareStructuredBuffer("RaySurfaceMaterials", state.gpu.surfaceMaterials, imported);
        for (size_t i = 0; i < state.gpu.readBuffers.size(); ++i) {
            const auto* buffer = resources.Get(state.gpu.readBuffers[i]);
            imported.byteSize = buffer ? buffer->GetSize() : 0;
            imported.stride = buffer ? buffer->GetStride() : 0;
            pipeline.DeclareBuffer("RayReadBuffer" + std::to_string(i), state.gpu.readBuffers[i], imported);
        }
        for (size_t i = 0; i < state.gpu.readTextures.size(); ++i) {
            const auto* texture = resources.Get(state.gpu.readTextures[i]);
            auto textureDesc = image;
            textureDesc.external = true;
            textureDesc.width = texture ? texture->GetWidth() : 0;
            textureDesc.height = texture ? texture->GetHeight() : 0;
            textureDesc.format = Format::RGBA8;
            pipeline.DeclareTexture("RayReadTexture" + std::to_string(i), state.gpu.readTextures[i], textureDesc);
        }
    }
    pipeline.AddPass<RayPathTracePass>(state, shared.rayPathShader);
    if (state.gameProfile) {
        pipeline.AddPass<RayGameReconstructionPass>(state, shared.rayGameReconstructionShader, false);
        pipeline.AddPass<RayGameReconstructionPass>(state, shared.rayGameReconstructionShader, true);
    }
    /// @note 最遠値 0 は通常の Reversed-Z 比較を通らないため、色を先に深度なしで全画素へ転写する。
    pipeline.AddRawPass("RayPathColor", {"RayPathResult"}, {"HDR"}, [&context, &state, &shared]() {
        context.renderer.SetRenderTarget(context.Res().Target("HDR"), context.resources);
        context.renderer.Clear({0, 0, 0, 1});
        if (!state.dispatchSucceeded) return;
        DrawCall draw;
        draw.shader = shared.copyColorShader;
        draw.pipelineState = shared.postprocPSO;
        draw.vertexCount = 3;
        draw.textures[5] = context.Res().Texture("RayPathResult");
        context.renderer.Submit(draw, context.resources);
    });
    /// @note 深度試験は一部画素だけを上書きするので、Color のクリアと背景転写を先行依存として保持する。
    pipeline.AddRawPass("RayPathResolve", std::vector<RenderGraph::ResourceAccess>{
        {"RayPathResult", Usage::Read}, {"RayPathSurface", Usage::Read}, {"HDR", Usage::ReadWrite}}, [&context, &state, &shared]() {
        if (!state.dispatchSucceeded) return;
        context.renderer.SetRenderTarget(context.Res().Target("HDR"), context.resources);
        DrawCall draw;
        draw.shader = shared.rayPathResolveShader;
        draw.pipelineState = shared.rayPathResolvePSO;
        draw.vertexCount = 3;
        draw.textures[5] = context.Res().Texture("RayPathResult");
        draw.textures[6] = context.Res().Texture("RayPathSurface");
        context.renderer.Submit(draw, context.resources);
    });
    if (context.settings.postProcess.bloom.enabled) {
        pipeline.DeclareTexture("Bloom", view.bloomFull, image);
        pipeline.AddPass<BloomPass>();
    }
    pipeline.AddPass<AutoExposurePass>();
    std::vector<RenderGraph::ResourceAccess> composite{{"HDR", Usage::Read}, {chain, Usage::Write}};
    if (context.settings.postProcess.bloom.enabled) composite.push_back({"Bloom", Usage::Read});
    pipeline.AddRawPass("Composite", std::move(composite), [&context]() { ExecuteCompositePass(context); });
    if (extensions.overlayDebug) extensions.overlayDebug(chain);
    if (upscale) pipeline.AddRawPass("Upscale", {chain}, {"Output"}, [&context]() { ExecuteUpscalePass(context); });
    if (extensions.ui) extensions.ui();
    if (state.gameProfile)
        pipeline.SetOutputs({"Output", "RayGameTransport", "RayGameSurface", "RayGameHistoryNext", "RayPathMaterial", "RayPathGeometry"});
    else pipeline.SetOutputs({"Output", "RayPathHistory", "RayPathIds", "RayPathMaterial", "RayPathGeometry"});
}

} /// @note namespace fbzz::renderer
