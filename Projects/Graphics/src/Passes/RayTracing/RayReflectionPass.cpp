/// @file    RayReflectionPass.cpp
/// @brief   一段反射の型付き geometry・表面・GBuffer 入力を記録する。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#include <Graphics/Renderer/Commands/ShaderBindingSlots.hpp>
#include <Graphics/Passes/RayTracing/RayReflectionPass.hpp>
#include <Graphics/Passes/RayTracing/RayDebugPass.hpp>
#include <Graphics/Pipeline/Resources/RenderResources.hpp>
#include <Graphics/Renderer/Commands/ComputeCall.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/Geometry/Mesh.hpp>
#include <cstddef>

namespace fbzz::renderer {
static_assert(sizeof(Vertex) == 60 && offsetof(Vertex, position) == 0
    && offsetof(Vertex, normal) == 12 && offsetof(Vertex, color) == 44);

void RayReflectionPass::Setup(PassBuilder& builder, const RenderPassContext& context) const
{
    using Purpose = RenderGraph::ResourceAccessPurpose;
    builder.Read("GBuffer", Purpose::SHADER_READ);
    if (context.IsHybridSsrPlanned()) {
        builder.Read("SSRResult", Purpose::SHADER_READ);
    }
    builder.Read("RayReflectionIrradiance", Purpose::SHADER_READ);
    builder.Read("RayReflectionPrefilter", Purpose::SHADER_READ);
    builder.Read("RayReflectionBrdfLut", Purpose::SHADER_READ);
    if (m_lighting.diffuseIndirectEnabled) {
        builder.Read("RayReflectionLightProbe0", Purpose::SHADER_READ);
        builder.Read("RayReflectionLightProbe1", Purpose::SHADER_READ);
        for (size_t slot = 0; slot < 2; ++slot) {
            if (context.handles.lightProbeVisibility[slot].IsValid()) {
                builder.Read("RayReflectionProbeVisibility" + std::to_string(slot), Purpose::SHADER_READ);
            }
        }
    }
    if (m_lighting.emitterCount) {
        builder.Read("RayReflectionEmitters", Purpose::SHADER_READ);
    }
    if (m_lighting.deltaLightCount) {
        builder.Read("RayReflectionDeltaLights", Purpose::SHADER_READ);
    }
    if (m_lighting.shapeCount) {
        builder.Read("RayReflectionShapes", Purpose::SHADER_READ);
    }
    if (m_lighting.environmentTableCount) {
        builder.Read("RayReflectionEnvironment", Purpose::SHADER_READ);
        builder.Read("RayReflectionEnvironmentTable", Purpose::SHADER_READ);
    }
    builder.Write(m_reconstruction ? "RayReflectionRaw" : "RayReflectionResult", Purpose::UAV);
    if (m_reconstruction && m_halfRaw.IsValid()) {
        builder.Write("RayReflectionHalfRaw", Purpose::UAV);
    }
    if (m_reconstruction) {
        builder.Write("RayReflectionSurface", Purpose::UAV);
    }
    if (m_scene.instanceCount) {
        builder.Read("RaySceneTLAS", Purpose::TRACE_READ);
        builder.Read("RayHitRecords", Purpose::SHADER_READ);
        builder.Read("RaySurfaceMaterials", Purpose::SHADER_READ);
        for (size_t i = 0; i < m_scene.readBuffers.size(); ++i) {
            builder.Read("RayReadBuffer" + std::to_string(i), Purpose::SHADER_READ);
        }
        for (size_t i = 0; i < m_scene.readTextures.size(); ++i) {
            builder.Read("RayReadTexture" + std::to_string(i), Purpose::SHADER_READ);
        }
    }
}

void RayReflectionPass::Execute(PassResources& resources, RenderPassContext& context)
{
    context.rayReflectionPassActive = false;
    if (m_reconstruction) {
        m_reconstruction->rawSucceeded = false;
        m_reconstruction->temporalSucceeded = false;
    }
    RayDebugConstants camera{};
    if (!MakeRayDebugCameraConstants(context.camera, camera)) {
        return;
    }
    RayReflectionConstants constants{};
    const auto quality = IsHybridQualityValid(context.settings.hybridQuality)
        ? context.settings.hybridQuality : HybridQualitySettings{};
    constants.sampleCount = quality.reflectionSamples;
    constants.cameraPosition = camera.cameraPosition;
    constants.cameraRight = camera.cameraRight;
    constants.cameraUp = camera.cameraUp;
    constants.cameraForward = camera.cameraForward;
    constants.orthographic = camera.orthographic;
    constants.nearDistance = camera.nearDistance;
    constants.farDistance = camera.farDistance;
    constants.maxDistance = quality.maxTraceDistance > 0 ? quality.maxTraceDistance : camera.farDistance;
    constants.traceDistanceLimited = quality.maxTraceDistance > 0 ? 1u : 0u;
    constants.width = context.width;
    constants.height = context.height;
    constants.traceWidth = context.width;
    constants.traceHeight = context.height;
    constants.instanceCount = m_scene.instanceCount;
    constants.incomplete = m_incomplete;
    constants.frameIndex = static_cast<uint32_t>(context.frameStamp);
    constants.sceneLighting = m_lighting.sceneLighting;
    constants.diffuseIndirectEnabled = m_lighting.diffuseIndirectEnabled ? 1u : 0u;
    constants.reflectionResolveEnabled = context.hybridReflectionResolveActive ? 1u : 0u;
    const auto ssrResult = context.IsHybridSsrPlanned() && context.ssrDispatchRecorded && !context.hybridReflectionSourcePass
        ? resources.Texture("SSRResult") : ResourceHandle<TextureTag>{};
    constants.reflectionSsrEnabled = context.resources.Get(ssrResult) ? 1u : 0u;
    constants.glassEnabled = m_lighting.glassEnabled ? 1u : 0u;
    constants.glassBoundaryLimit = constants.glassEnabled ? quality.glassBoundaryLimit : 0u;
    constants.hybridPolicyFlags = (m_scene.hybridCandidatePolicy ? 1u : 0u) | (quality.glassStochastic ? 2u : 0u);
    // カメラが変わった場合や正射影の画素別原点では、準備時の透視投影による証明を再利用できない。
    constants.cameraOriginProvenAir = constants.glassEnabled && !camera.orthographic
        && m_lighting.cameraOriginProvenAir
        && m_lighting.provenAirOrigin.x == camera.cameraPosition.x
        && m_lighting.provenAirOrigin.y == camera.cameraPosition.y
        && m_lighting.provenAirOrigin.z == camera.cameraPosition.z ? 1u : 0u;
    constants.emitterCount = m_lighting.emitterCount;
    constants.deltaLightCount = m_lighting.deltaLightCount;
    constants.shapeCount = m_lighting.shapeCount;
    constants.environmentTableCount = m_lighting.environmentTableCount;
    constants.environmentFaceSize = m_lighting.environmentFaceSize;
    constants.environmentRotation = m_lighting.environmentRotation;
    constants.environmentIntensity = m_lighting.environmentIntensity;
    constants.environmentMode = m_lighting.environmentTableCount ? 2u : (m_lighting.constantEnvironmentKnown ? 1u : 0u);
    constants.environmentRadiance[0] = m_lighting.constantEnvironmentRadiance.x;
    constants.environmentRadiance[1] = m_lighting.constantEnvironmentRadiance.y;
    constants.environmentRadiance[2] = m_lighting.constantEnvironmentRadiance.z;
    constants.writeMetadata = m_reconstruction ? 1u : 0u;
    constants.jitterNdcX = context.taaJitterNdcX;
    constants.jitterNdcY = context.taaJitterNdcY;
    const auto& light = context.lightData;
    constants.lightDirection = {light.lightDir.x, light.lightDir.y, light.lightDir.z, context.shadowStrength};
    constants.lightColorIntensity = {light.lightColor.x, light.lightColor.y, light.lightColor.z, light.lightIntensity};
    constants.ambientRadiance = {light.ambientColor.x, light.ambientColor.y, light.ambientColor.z, 0};
    ComputeCall call;
    call.shader = m_shader;
    call.constantBuffers[cb::RAY_REFLECTION] = m_constants;
    call.constantBuffers[cb::ADVANCED_GRAPHICS] = context.handles.advancedGraphicsCB;
    call.uavOutputs[uav::RAY_RESULT] = resources.Texture(m_reconstruction ? "RayReflectionRaw" : "RayReflectionResult");
    if (m_reconstruction && m_halfRaw.IsValid()) {
        const auto halfRawHandle = resources.Texture("RayReflectionHalfRaw");
        const auto* halfRaw = context.resources.Get(halfRawHandle);
        if (halfRaw && halfRaw->GetWidth() == context.width / 2 + context.width % 2
            && halfRaw->GetHeight() == context.height / 2 + context.height % 2
            && halfRaw->GetBindlessIndex() != INVALID_BINDLESS_INDEX
            && halfRaw->GetBindlessUavIndex() != INVALID_BINDLESS_INDEX) {
            call.uavOutputs[uav::RAY_REFLECTION_HALF] = halfRawHandle;
            constants.traceWidth = halfRaw->GetWidth();
            constants.traceHeight = halfRaw->GetHeight();
        }
    }
    context.handles.rayReflectionResult = call.uavOutputs[uav::RAY_RESULT];
    if (m_reconstruction) {
        call.uavBuffers[StructuredUavIndex(uav::RAY_REFLECTION_METADATA)] = resources.StructuredBuffer("RayReflectionSurface");
    }
    const auto gbuffer = resources.Target("GBuffer");
    call.srvInputs[srv::GBUFFER0] = context.resources.GetColorTexture(gbuffer, 0);
    call.srvInputs[srv::GBUFFER1] = context.resources.GetColorTexture(gbuffer, 1);
    call.srvInputs[srv::DEPTH] = context.resources.GetDepthTexture(gbuffer);
    if (constants.glassEnabled) {
        call.srvInputs[srv::GBUFFER_EMISSION] = context.resources.GetColorTexture(gbuffer, 2);
    }
    if (constants.reflectionSsrEnabled) {
        call.srvInputs[srv::SCREEN_REFLECTION] = ssrResult;
    }
    call.srvInputs[srv::IBL_IRRADIANCE] = resources.Texture("RayReflectionIrradiance");
    call.srvInputs[srv::IBL_PREFILTER] = resources.Texture("RayReflectionPrefilter");
    call.srvInputs[srv::IBL_BRDF_LUT] = resources.Texture("RayReflectionBrdfLut");
    if (constants.diffuseIndirectEnabled) {
        call.srvInputs[srv::RAY_REFLECTION_PROBE] = resources.Texture("RayReflectionLightProbe0");
        call.srvInputs[srv::RAY_REFLECTION_PROBE_OUTER] = resources.Texture("RayReflectionLightProbe1");
        for (size_t slot = 0; slot < 2; ++slot) {
            if (const auto texture = resources.Texture("RayReflectionProbeVisibility" + std::to_string(slot)); texture.IsValid()) {
                call.indirectReadTextures.push_back(texture);
            }
        }
    }
    if (m_lighting.emitterCount) {
        call.srvBuffers[srv::RAY_EMITTERS] = resources.StructuredBuffer("RayReflectionEmitters");
    }
    if (m_lighting.deltaLightCount) {
        call.srvBuffers[srv::RAY_REFLECTION_LIGHTS] = resources.StructuredBuffer("RayReflectionDeltaLights");
    }
    if (m_lighting.shapeCount) {
        call.srvBuffers[srv::RAY_REFLECTION_SHAPES] = resources.StructuredBuffer("RayReflectionShapes");
    }
    if (m_lighting.environmentTableCount) {
        call.srvInputs[srv::RAY_REFLECTION_ENVIRONMENT] = resources.Texture("RayReflectionEnvironment");
        call.srvBuffers[srv::RAY_REFLECTION_ENV_TABLE] = resources.StructuredBuffer("RayReflectionEnvironmentTable");
    }
    constants.iblReady = call.constantBuffers[cb::ADVANCED_GRAPHICS].IsValid() && call.srvInputs[srv::IBL_IRRADIANCE].IsValid()
        && call.srvInputs[srv::IBL_PREFILTER].IsValid() && call.srvInputs[srv::IBL_BRDF_LUT].IsValid();
    context.resources.Update(m_constants, &constants, sizeof(constants));
    call.dispatchX = (context.width + 7) / 8;
    call.dispatchY = (context.height + 7) / 8;
    if (m_scene.instanceCount) {
        call.accelerationStructures[srv::RAY_SCENE] = resources.AccelerationStructure("RaySceneTLAS");
        call.srvBuffers[srv::RAY_HIT_RECORDS] = resources.StructuredBuffer("RayHitRecords");
        call.srvBuffers[srv::RAY_MATERIALS] = resources.StructuredBuffer("RaySurfaceMaterials");
        for (size_t i = 0; i < m_scene.readBuffers.size(); ++i) {
            call.indirectReadBuffers.push_back(resources.Buffer("RayReadBuffer" + std::to_string(i)));
        }
        for (size_t i = 0; i < m_scene.readTextures.size(); ++i) {
            call.indirectReadTextures.push_back(resources.Texture("RayReadTexture" + std::to_string(i)));
        }
    }
    if (!call.shader.IsValid() || !call.uavOutputs[uav::RAY_RESULT].IsValid() || !gbuffer.IsValid()
        || !call.srvInputs[srv::GBUFFER0].IsValid() || !call.srvInputs[srv::GBUFFER1].IsValid() || !call.srvInputs[srv::DEPTH].IsValid()
        || (constants.glassEnabled && !context.resources.Get(call.srvInputs[srv::GBUFFER_EMISSION]))) {
        return;
    }
    context.rayReflectionPassActive = context.renderer.TryDispatch(call, context.resources);
    if (m_reconstruction) {
        m_reconstruction->rawSucceeded = context.rayReflectionPassActive;
        if (!context.rayReflectionPassActive) {
            m_reconstruction->historyValid = false;
        }
    }
}

}
