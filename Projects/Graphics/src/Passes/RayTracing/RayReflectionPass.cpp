/// @file    RayReflectionPass.cpp
/// @brief   一段反射の型付き geometry・表面・GBuffer 入力を記録する。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#include <Graphics/Passes/RayTracing/RayReflectionPass.hpp>
#include <Graphics/Passes/RayTracing/RayDebugPass.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/Mesh.hpp>
#include <cstddef>

namespace fbzz::renderer {
static_assert(sizeof(Vertex) == 60 && offsetof(Vertex, position) == 0
    && offsetof(Vertex, normal) == 12 && offsetof(Vertex, color) == 44);

void RayReflectionPass::Setup(PassBuilder& builder, const RenderPassContext& context) const
{
    using Purpose = RenderGraph::ResourceAccessPurpose;
    builder.Read("GBuffer", Purpose::SHADER_READ);
    if (context.hybridReflectionSsrPlanned) builder.Read("SSRResult", Purpose::SHADER_READ);
    builder.Read("RayReflectionIrradiance", Purpose::SHADER_READ);
    builder.Read("RayReflectionPrefilter", Purpose::SHADER_READ);
    builder.Read("RayReflectionBrdfLut", Purpose::SHADER_READ);
    if (m_lighting.diffuseIndirectEnabled) {
        builder.Read("RayReflectionLightProbe0", Purpose::SHADER_READ);
        builder.Read("RayReflectionLightProbe1", Purpose::SHADER_READ);
    }
    if (m_lighting.emitterCount) builder.Read("RayReflectionEmitters", Purpose::SHADER_READ);
    if (m_lighting.deltaLightCount) builder.Read("RayReflectionDeltaLights", Purpose::SHADER_READ);
    if (m_lighting.shapeCount) builder.Read("RayReflectionShapes", Purpose::SHADER_READ);
    if (m_lighting.environmentTableCount) {
        builder.Read("RayReflectionEnvironment", Purpose::SHADER_READ);
        builder.Read("RayReflectionEnvironmentTable", Purpose::SHADER_READ);
    }
    builder.Write(m_reconstruction ? "RayReflectionRaw" : "RayReflectionResult", Purpose::UAV);
    if (m_reconstruction) builder.Write("RayReflectionSurface", Purpose::UAV);
    if (m_scene.instanceCount) {
        builder.Read("RaySceneTLAS", Purpose::TRACE_READ);
        builder.Read("RayHitRecords", Purpose::SHADER_READ);
        builder.Read("RaySurfaceMaterials", Purpose::SHADER_READ);
        for (size_t i = 0; i < m_scene.readBuffers.size(); ++i)
            builder.Read("RayReadBuffer" + std::to_string(i), Purpose::SHADER_READ);
        for (size_t i = 0; i < m_scene.readTextures.size(); ++i)
            builder.Read("RayReadTexture" + std::to_string(i), Purpose::SHADER_READ);
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
    if (!MakeRayDebugCameraConstants(context.camera, camera)) return;
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
    constants.instanceCount = m_scene.instanceCount;
    constants.incomplete = m_incomplete;
    constants.frameIndex = static_cast<uint32_t>(context.frameStamp);
    constants.sceneLighting = m_lighting.sceneLighting;
    constants.diffuseIndirectEnabled = m_lighting.diffuseIndirectEnabled ? 1u : 0u;
    constants.reflectionResolveEnabled = context.hybridReflectionResolveActive ? 1u : 0u;
    constants.reflectionSsrEnabled = context.hybridReflectionResolveActive && context.ssrPassActive
        && !context.hybridReflectionSourcePass ? 1u : 0u;
    constants.glassEnabled = m_lighting.glassEnabled ? 1u : 0u;
    constants.glassBoundaryLimit = constants.glassEnabled ? quality.glassBoundaryLimit : 0u;
    /// @note A changed camera or orthographic pixel origins cannot reuse a preparation-time perspective proof.
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
    call.constantBuffers[0] = m_constants;
    call.constantBuffers[8] = context.handles.advancedGraphicsCB;
    call.uavOutputs[0] = resources.Texture(m_reconstruction ? "RayReflectionRaw" : "RayReflectionResult");
    context.handles.rayReflectionResult = call.uavOutputs[0];
    if (m_reconstruction) call.uavBuffers[0] = resources.StructuredBuffer("RayReflectionSurface");
    const auto gbuffer = resources.Target("GBuffer");
    call.srvInputs[5] = context.resources.GetColorTexture(gbuffer, 0);
    call.srvInputs[6] = context.resources.GetColorTexture(gbuffer, 1);
    call.srvInputs[7] = context.resources.GetDepthTexture(gbuffer);
    if (constants.glassEnabled) call.srvInputs[22] = context.resources.GetColorTexture(gbuffer, 2);
    if (constants.reflectionSsrEnabled) call.srvInputs[23] = resources.Texture("SSRResult");
    call.srvInputs[16] = resources.Texture("RayReflectionIrradiance");
    call.srvInputs[17] = resources.Texture("RayReflectionPrefilter");
    call.srvInputs[18] = resources.Texture("RayReflectionBrdfLut");
    if (constants.diffuseIndirectEnabled) {
        call.srvInputs[11] = resources.Texture("RayReflectionLightProbe0");
        call.srvInputs[12] = resources.Texture("RayReflectionLightProbe1");
    }
    if (m_lighting.emitterCount) call.srvBuffers[3] = resources.StructuredBuffer("RayReflectionEmitters");
    if (m_lighting.deltaLightCount) call.srvBuffers[4] = resources.StructuredBuffer("RayReflectionDeltaLights");
    if (m_lighting.shapeCount) call.srvBuffers[10] = resources.StructuredBuffer("RayReflectionShapes");
    if (m_lighting.environmentTableCount) {
        call.srvInputs[8] = resources.Texture("RayReflectionEnvironment");
        call.srvBuffers[9] = resources.StructuredBuffer("RayReflectionEnvironmentTable");
    }
    constants.iblReady = call.constantBuffers[8].IsValid() && call.srvInputs[16].IsValid()
        && call.srvInputs[17].IsValid() && call.srvInputs[18].IsValid();
    context.resources.Update(m_constants, &constants, sizeof(constants));
    call.dispatchX = (context.width + 7) / 8;
    call.dispatchY = (context.height + 7) / 8;
    if (m_scene.instanceCount) {
        call.accelerationStructures[0] = resources.AccelerationStructure("RaySceneTLAS");
        call.srvBuffers[1] = resources.StructuredBuffer("RayHitRecords");
        call.srvBuffers[2] = resources.StructuredBuffer("RaySurfaceMaterials");
        for (size_t i = 0; i < m_scene.readBuffers.size(); ++i)
            call.indirectReadBuffers.push_back(resources.Buffer("RayReadBuffer" + std::to_string(i)));
        for (size_t i = 0; i < m_scene.readTextures.size(); ++i)
            call.indirectReadTextures.push_back(resources.Texture("RayReadTexture" + std::to_string(i)));
    }
    if (!call.shader.IsValid() || !call.uavOutputs[0].IsValid() || !gbuffer.IsValid()
        || !call.srvInputs[5].IsValid() || !call.srvInputs[6].IsValid() || !call.srvInputs[7].IsValid()
        || (constants.glassEnabled && !context.resources.Get(call.srvInputs[22]))) return;
    context.rayReflectionPassActive = context.renderer.TryDispatch(call, context.resources);
    if (m_reconstruction) {
        m_reconstruction->rawSucceeded = context.rayReflectionPassActive;
        if (!context.rayReflectionPassActive) m_reconstruction->historyValid = false;
    }
}

} /// @note namespace fbzz::renderer
