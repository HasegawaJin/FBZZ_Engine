/// @file    RayReflectionReconstructionResources.cpp
/// @brief   完全な内容キーと検証済み camera-motion 履歴を準備する。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#include <Graphics/RayTracing/RayReflectionReconstructionResources.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Graphics/Passes/RayTracing/RayDebugPass.hpp>
#include <Graphics/Renderer/IConstantBuffer.hpp>
#include <Graphics/Renderer/IStructuredBuffer.hpp>
#include <bit>
#include <limits>

namespace fbzz::renderer {
namespace {
void AppendVersion(std::vector<uint32_t>& key, uint64_t value)
{
    key.push_back(static_cast<uint32_t>(value));
    key.push_back(static_cast<uint32_t>(value >> 32));
}
void AppendFloat(std::vector<uint32_t>& key, float value) { key.push_back(std::bit_cast<uint32_t>(value)); }
void AppendTexture(std::vector<uint32_t>& key, ResourceHandle<TextureTag> handle,
    ResourceManager& resources, bool& knownContent, const RenderTexturePublication* publication = nullptr)
{
    key.push_back(handle.id); key.push_back(handle.gen);
    const auto* texture = resources.Get(handle);
    const auto version = texture ? texture->GetContentVersion() : 0;
    AppendVersion(key, version);
    if (handle.IsValid() && !texture) knownContent = false;
    if (texture && !version && (!publication || !publication->Matches(handle, &resources, resources.GetResetVersion())))
        knownContent = false;
}
} /// @note anonymous namespace

bool PrepareRayReflectionReconstruction(RenderPassContext& context, RenderViewResources& view,
    RenderSharedResources& shared)
{
    auto& state = view.rayReflection.reconstruction;
    auto& resources = context.resources;
    state.prepared = state.rawSucceeded = state.temporalSucceeded = false;
    context.rayReflectionReconstructionPrepared = false;
    const auto reject = [&]() { state.historyValid = false; return false; };
    if (!context.experimentalRayTracingEnabled) return reject();
    for (const auto& override : context.settings.passOverrides)
        if (!override.enabled && (override.name == "RayReflectionTemporal" || override.name == "RayReflectionSpatial"))
            return reject();
    const uint64_t pixelCount = static_cast<uint64_t>(context.width) * context.height;
    if (!pixelCount || pixelCount > std::numeric_limits<uint32_t>::max()) return reject();
    const auto& quality = context.settings.hybridQuality;
    const bool halfResolution = view.rayReflection.resolutionDivisor == 2;
    const uint32_t traceWidth = halfResolution ? context.width / 2 + context.width % 2 : context.width;
    const uint32_t traceHeight = halfResolution ? context.height / 2 + context.height % 2 : context.height;
    const uint64_t tracePixelCount = static_cast<uint64_t>(traceWidth) * traceHeight;
    /// @note This is logical live reconstruction storage, not a claim about driver heaps or fence-retired allocations.
    constexpr uint64_t bytesPerPixel = 2 * sizeof(RayReflectionSurface) + 2 * sizeof(RayReflectionHistoryRecord) + 8;
    const uint64_t workingSetBytes = pixelCount * bytesPerPixel + (halfResolution ? tracePixelCount * 8 : 0);
    if (!IsHybridQualityValid(quality) || workingSetBytes > static_cast<uint64_t>(quality.maxHistoryMiB) * 1024 * 1024) {
        for (const auto surface : state.surfaces) resources.Release(surface);
        for (const auto history : state.histories) resources.Release(history);
        resources.Release(state.output);
        state.surfaces = {}; state.histories = {}; state.output = {};
        state.width = state.height = state.surfaceReadIndex = 0;
        return reject();
    }
    /// @note Half RAW cannot be expanded without current-frame GBuffer material/normal checks for every Scene pixel.
    if (halfResolution && !resources.Get(view.gbuffer)) return reject();
    if (!resources.Get(shared.rayReflectionReconstructionShader))
        shared.rayReflectionReconstructionShader = resources.LoadShader("Assets/Shaders/RayTracing/RayReflectionReconstruction.cs.hlsl");
    if (!resources.Get(shared.rayReflectionReconstructionShader)) return reject();
    const bool resize = state.width != context.width || state.height != context.height;
    const auto storageReady = [&](ResourceHandle<StructuredBufferTag> handle, uint32_t stride) {
        const auto* buffer = resources.Get(handle);
        return buffer && buffer->GetStride() == stride && buffer->GetElementCount() >= pixelCount;
    };
    if (resize || !storageReady(state.surfaces[0], sizeof(RayReflectionSurface))
        || !storageReady(state.surfaces[1], sizeof(RayReflectionSurface))
        || !storageReady(state.histories[0], sizeof(RayReflectionHistoryRecord))
        || !storageReady(state.histories[1], sizeof(RayReflectionHistoryRecord)) || !resources.Get(state.output)) {
        for (const auto surface : state.surfaces) resources.Release(surface);
        for (const auto history : state.histories) resources.Release(history);
        resources.Release(state.output);
        state.surfaces = {}; state.histories = {}; state.output = {};
        state.historyValid = false; state.surfaceReadIndex = 0;
        const auto count = static_cast<uint32_t>(pixelCount);
        /// @note Null initial data avoids an in-frame upload/Flush; resetHistory writes every output record before reading it.
        for (auto& surface : state.surfaces)
            surface = resources.CreateRWStructuredBuffer(nullptr, count, sizeof(RayReflectionSurface));
        for (auto& history : state.histories)
            history = resources.CreateRWStructuredBuffer(nullptr, count, sizeof(RayReflectionHistoryRecord));
        state.output = resources.CreateComputeTexture(context.width, context.height);
        state.width = context.width; state.height = context.height;
    }
    const auto* currentConstants = resources.Get(state.constants);
    if (!currentConstants || currentConstants->GetSize() < sizeof(RayReflectionReconstructionConstants)) {
        resources.Release(state.constants);
        state.constants = resources.CreateConstantBuffer(sizeof(RayReflectionReconstructionConstants));
    }
    const auto bufferReady = [&](ResourceHandle<StructuredBufferTag> handle, uint32_t stride) {
        const auto* buffer = resources.Get(handle);
        return storageReady(handle, stride)
            && buffer->GetBindlessIndex() != INVALID_BINDLESS_INDEX
            && buffer->GetBindlessUavIndex() != INVALID_BINDLESS_INDEX;
    };
    const auto* output = resources.Get(state.output);
    if (!resources.Get(state.constants) || state.surfaceReadIndex >= state.surfaces.size()
        || !bufferReady(state.surfaces[0], sizeof(RayReflectionSurface))
        || !bufferReady(state.surfaces[1], sizeof(RayReflectionSurface))
        || !bufferReady(state.histories[0], sizeof(RayReflectionHistoryRecord))
        || !bufferReady(state.histories[1], sizeof(RayReflectionHistoryRecord)) || !output
        || output->GetBindlessIndex() == INVALID_BINDLESS_INDEX || output->GetBindlessUavIndex() == INVALID_BINDLESS_INDEX)
        return reject();
    RayDebugConstants basis{};
    if (!MakeRayDebugCameraConstants(context.camera, basis)) return reject();
    std::vector<uint32_t> key;
    AppendVersion(key, view.rayReflection.scene.sceneGeneration);
    AppendVersion(key, view.rayReflection.pathScene.contentRevision);
    AppendVersion(key, resources.GetShaderVersion());
    AppendVersion(key, resources.GetResetVersion());
    key.push_back(context.width); key.push_back(context.height); key.push_back(context.cullingMask);
    const auto& camera = context.camera;
    for (float value : {camera.m_fovY, camera.m_aspect, camera.m_near, camera.m_far, camera.m_orthoHeight}) AppendFloat(key, value);
    key.push_back(static_cast<uint32_t>(camera.m_projection));
    key.push_back(quality.reflectionSamples); key.push_back(quality.historyLimit); key.push_back(quality.spatialRadius);
    key.push_back(quality.glassBoundaryLimit); AppendFloat(key, quality.maxTraceDistance);
    key.push_back(quality.reflectionResolutionDivisor); key.push_back(quality.glassStochastic ? 1u : 0u);
    key.push_back(view.rayReflection.resolutionDivisor); key.push_back(traceWidth); key.push_back(traceHeight);
    std::vector<uint32_t> cameraKey;
    for (float value : {camera.m_position.x, camera.m_position.y, camera.m_position.z,
        camera.m_rotation.x, camera.m_rotation.y, camera.m_rotation.z, camera.m_rotation.w}) AppendFloat(cameraKey, value);
    /// @note Object/geometry/light/optical changes remain exact whole-history resets; camera pose alone may use proven terminal correspondence.
    bool knownContent = true;
    AppendTexture(key, view.rayReflection.environment, resources, knownContent);
    const bool rasterEnvironmentUsed = !view.rayReflection.environment.IsValid()
        && !view.rayReflection.constantEnvironmentKnown;
    const bool iblBound = resources.Get(context.handles.advancedGraphicsCB)
        && resources.Get(context.handles.iblIrradiance) && resources.Get(context.handles.iblPrefilter)
        && resources.Get(context.handles.iblBrdfLut);
    if (rasterEnvironmentUsed && iblBound && !view.advancedGraphicsSnapshotValid) knownContent = false;
    const bool iblUsed = rasterEnvironmentUsed && iblBound
        && view.advancedGraphicsSnapshotValid && view.advancedGraphicsSnapshot.iblIntensity > 0;
    key.push_back(rasterEnvironmentUsed ? 1u : 0u);
    key.push_back(iblUsed ? 1u : 0u);
    if (iblUsed) {
        AppendTexture(key, context.handles.iblIrradiance, resources, knownContent, &context.iblIrradiancePublication);
        AppendTexture(key, context.handles.iblPrefilter, resources, knownContent, &context.iblPrefilterPublication);
        AppendTexture(key, context.handles.iblBrdfLut, resources, knownContent);
        for (float value : {view.advancedGraphicsSnapshot.iblIntensity, view.advancedGraphicsSnapshot.iblDiffuseScale,
            view.advancedGraphicsSnapshot.iblSpecularScale}) AppendFloat(key, value);
        key.push_back(static_cast<uint32_t>(view.advancedGraphicsSnapshot.iblMaxMipLevel));
    } else if (rasterEnvironmentUsed) {
        for (float value : {context.lightData.ambientColor.x, context.lightData.ambientColor.y,
            context.lightData.ambientColor.z}) AppendFloat(key, value);
    }
    key.push_back(view.rayReflection.constantEnvironmentKnown ? 1u : 0u);
    for (float value : {view.rayReflection.constantEnvironmentRadiance.x, view.rayReflection.constantEnvironmentRadiance.y,
        view.rayReflection.constantEnvironmentRadiance.z}) AppendFloat(key, value);
    const bool diffuseIndirect = view.rayReflection.diffuseIndirectEnabled;
    key.push_back(diffuseIndirect ? 1u : 0u);
    if (diffuseIndirect) {
        if (!view.advancedGraphicsSnapshotValid || !resources.Get(context.handles.advancedGraphicsCB)
            || !resources.Get(context.handles.iblIrradiance)) knownContent = false;
        AppendTexture(key, context.handles.iblIrradiance, resources, knownContent, &context.iblIrradiancePublication);
        AppendFloat(key, view.advancedGraphicsSnapshot.iblIntensity);
        AppendFloat(key, view.advancedGraphicsSnapshot.iblDiffuseScale);
        for (uint32_t slot = 0; slot < 2; ++slot) {
            const auto& probe = view.advancedGraphicsSnapshot.probeVolumes[slot];
            const bool active = probe.intensity > 0;
            key.push_back(active ? 1u : 0u);
            if (!active) continue;
            if (!resources.Get(context.handles.lightProbeSH[slot])) knownContent = false;
            AppendTexture(key, context.handles.lightProbeSH[slot], resources, knownContent);
            for (float value : {probe.boxMin.x, probe.boxMin.y, probe.boxMin.z, probe.intensity,
                probe.invSize.x, probe.invSize.y, probe.invSize.z, probe.fade, probe.normalBias}) AppendFloat(key, value);
            for (uint32_t count : probe.grid) key.push_back(count);
        }
    }
    /// @note GI consumes irradiance/probes independently of the actual environment; unversioned writable providers reject history unless an immutable publication proves their content.
    const bool uninterrupted = context.frameStamp > state.lastFrameStamp && context.frameStamp - state.lastFrameStamp == 1;
    const bool cameraMotion = state.cameraKey != cameraKey;
    const bool cameraCut = state.historyValid && cameraMotion
        && math::Vector3::Dot(basis.cameraForward.XYZ(), state.committedCamera.cameraForward.XYZ()) <= 0;
    const bool reset = !state.historyValid || !uninterrupted || state.contentKey != key || !knownContent || cameraCut;
    state.contentKey = std::move(key);
    state.cameraKey = std::move(cameraKey);
    auto& constants = state.constantsData;
    constants = {};
    constants.cameraPosition = basis.cameraPosition; constants.cameraPosition.w = static_cast<float>(basis.orthographic);
    constants.cameraRight = basis.cameraRight; constants.cameraUp = basis.cameraUp; constants.cameraForward = basis.cameraForward;
    constants.width = context.width; constants.height = context.height;
    constants.traceWidth = traceWidth; constants.traceHeight = traceHeight;
    constants.resolutionDivisor = view.rayReflection.resolutionDivisor;
    constants.resetHistory = reset ? 1u : 0u;
    constants.temporalAllowed = knownContent ? 1u : 0u;
    constants.historyLimit = quality.historyLimit;
    constants.spatialRadius = quality.spatialRadius;
    constants.previousCameraPosition = state.committedCamera.cameraPosition;
    constants.previousCameraRight = state.committedCamera.cameraRight;
    constants.previousCameraUp = state.committedCamera.cameraUp;
    constants.previousCameraForward = state.committedCamera.cameraForward;
    constants.constantEnvironmentRadiance = {view.rayReflection.constantEnvironmentRadiance.x,
        view.rayReflection.constantEnvironmentRadiance.y, view.rayReflection.constantEnvironmentRadiance.z,
        view.rayReflection.constantEnvironmentKnown ? 1.0f : 0.0f};
    constants.nearDistance = camera.m_near; constants.farDistance = camera.m_far;
    constants.cameraMotion = cameraMotion ? 1u : 0u;
    constants.previousJitterX = state.previousJitterX; constants.previousJitterY = state.previousJitterY;
    constants.currentJitterX = context.taaJitterNdcX; constants.currentJitterY = context.taaJitterNdcY;
    constants.hasGBuffer = resources.Get(view.gbuffer) ? 1u : 0u;
    state.prepared = true;
    context.rayReflectionReconstructionPrepared = true;
    return true;
}
} /// @note namespace fbzz::renderer
