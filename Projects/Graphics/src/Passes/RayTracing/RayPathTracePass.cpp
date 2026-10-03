/// @file    RayPathTracePass.cpp
/// @brief   型付き Path 資源と RAW UAV 履歴の依存を記録する。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#include <Graphics/Passes/RayTracing/RayPathTracePass.hpp>
#include <Graphics/Passes/RayTracing/RayDebugPass.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Graphics/Renderer/IStructuredBuffer.hpp>

namespace fbzz::renderer {

bool MakeRayPathCameraConstants(const Camera& camera, RayPathTraceConstants& output)
{
    RayDebugConstants constants{};
    if (!MakeRayDebugCameraConstants(camera, constants)) return false;
    output.cameraPosition = constants.cameraPosition;
    output.cameraRight = constants.cameraRight;
    output.cameraUp = constants.cameraUp;
    output.cameraForward = constants.cameraForward;
    output.nearDistance = constants.nearDistance;
    output.farDistance = constants.farDistance;
    output.orthographic = constants.orthographic;
    return true;
}

void RayPathTracePass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    using Purpose = RenderGraph::ResourceAccessPurpose;
    if (m_state.gameProfile) {
        builder.Read("GBuffer", Purpose::SHADER_READ);
        builder.Write("RayGameTransport", Purpose::UAV);
        builder.Write("RayGameSurface", Purpose::UAV);
        if (m_state.gpu.instanceCount) builder.Read("RayGameMotionInstances", Purpose::SHADER_READ);
    } else {
        builder.ReadWrite("RayPathHistory", Purpose::UAV);
        builder.Write("RayPathIds", Purpose::UAV);
    }
    for (const char* name : {"RayPathResult", "RayPathSurface", "RayPathMaterial", "RayPathGeometry"})
        builder.Write(name, Purpose::UAV);
    builder.Read("RayPathEnvironment", Purpose::SHADER_READ);
    if (m_state.constantsData.emitterCount) builder.Read("RayPathEmitters", Purpose::SHADER_READ);
    if (m_state.constantsData.deltaLightCount) builder.Read("RayPathDeltaLights", Purpose::SHADER_READ);
    if (m_state.constantsData.shapeLightCount) builder.Read("RayPathShapes", Purpose::SHADER_READ);
    if (m_state.constantsData.environmentTableCount) builder.Read("RayPathEnvironmentTable", Purpose::SHADER_READ);
    if (m_state.gpu.instanceCount) {
        builder.Read("RaySceneTLAS", Purpose::TRACE_READ);
        builder.Read("RayHitRecords", Purpose::SHADER_READ);
        builder.Read("RaySurfaceMaterials", Purpose::SHADER_READ);
        for (size_t i = 0; i < m_state.gpu.readBuffers.size(); ++i)
            builder.Read("RayReadBuffer" + std::to_string(i), Purpose::SHADER_READ);
        for (size_t i = 0; i < m_state.gpu.readTextures.size(); ++i)
            builder.Read("RayReadTexture" + std::to_string(i), Purpose::SHADER_READ);
    }
}

void RayPathTracePass::Execute(PassResources& resources, RenderPassContext& context)
{
    m_state.dispatchSucceeded = false;
    context.rayPathPassActive = false;
    auto constants = m_state.constantsData;
    if (!MakeRayPathCameraConstants(context.camera, constants)
        || constants.samplesPerDispatch == 0 || constants.samplesPerDispatch > 64
        || constants.sampleBase > UINT32_MAX - 1u - constants.samplesPerDispatch) return;
    constants.width = context.width;
    constants.height = context.height;
    constants.instanceCount = m_state.gpu.instanceCount;
    ComputeCall call;
    call.shader = m_shader;
    call.constantBuffers[0] = m_state.constants;
    call.uavOutputs[0] = resources.Texture("RayPathResult");
    call.uavOutputs[1] = resources.Texture("RayPathSurface");
    call.uavOutputs[4] = resources.Texture("RayPathMaterial");
    call.uavOutputs[5] = resources.Texture("RayPathGeometry");
    if (m_state.gameProfile) {
        call.constantBuffers[1] = m_state.game.traceConstants;
        call.uavBuffers[0] = resources.StructuredBuffer("RayGameTransport");
        call.uavBuffers[1] = resources.StructuredBuffer("RayGameSurface");
        const auto target = resources.Target("GBuffer");
        call.srvInputs[8] = context.resources.GetColorTexture(target, 0);
        call.srvInputs[9] = context.resources.GetColorTexture(target, 1);
        call.srvInputs[10] = context.resources.GetDepthTexture(target);
        call.srvInputs[11] = context.resources.GetColorTexture(target, 2);
        if (constants.instanceCount) call.srvBuffers[12] = resources.StructuredBuffer("RayGameMotionInstances");
        if (!context.resources.Get(call.constantBuffers[1])) return;
        for (const uint32_t slot : {8u, 9u, 10u, 11u}) if (!context.resources.Get(call.srvInputs[slot])) return;
        if (constants.instanceCount && !context.resources.Get(call.srvBuffers[12])) return;
        context.resources.Update(m_state.game.traceConstants, &m_state.game.traceData, sizeof(RayGameTraceConstants));
    } else {
        call.uavBuffers[0] = resources.StructuredBuffer("RayPathHistory");
        call.uavBuffers[1] = resources.StructuredBuffer("RayPathIds");
    }
    call.srvInputs[4] = resources.Texture("RayPathEnvironment");
    if (constants.environmentMode == 2 && !context.resources.Get(call.srvInputs[4])) return;
    if (constants.emitterCount) call.srvBuffers[3] = resources.StructuredBuffer("RayPathEmitters");
    if (constants.deltaLightCount) call.srvBuffers[5] = resources.StructuredBuffer("RayPathDeltaLights");
    if (constants.shapeLightCount) call.srvBuffers[6] = resources.StructuredBuffer("RayPathShapes");
    if (constants.environmentTableCount) call.srvBuffers[7] = resources.StructuredBuffer("RayPathEnvironmentTable");
    if ((constants.emitterCount && !context.resources.Get(call.srvBuffers[3]))
        || (constants.deltaLightCount && !context.resources.Get(call.srvBuffers[5]))
        || (constants.shapeLightCount && !context.resources.Get(call.srvBuffers[6]))
        || (constants.environmentTableCount && !context.resources.Get(call.srvBuffers[7]))) return;
    if (constants.instanceCount) {
        call.accelerationStructures[0] = resources.AccelerationStructure("RaySceneTLAS");
        call.srvBuffers[1] = resources.StructuredBuffer("RayHitRecords");
        call.srvBuffers[2] = resources.StructuredBuffer("RaySurfaceMaterials");
        for (size_t i = 0; i < m_state.gpu.readBuffers.size(); ++i)
            call.indirectReadBuffers.push_back(resources.Buffer("RayReadBuffer" + std::to_string(i)));
        for (size_t i = 0; i < m_state.gpu.readTextures.size(); ++i)
            call.indirectReadTextures.push_back(resources.Texture("RayReadTexture" + std::to_string(i)));
    }
    if (!context.width || !context.height || !context.resources.Get(m_shader)
        || !context.resources.Get(m_state.constants)) return;
    for (const uint32_t slot : {0u, 1u, 4u, 5u})
        if (!context.resources.Get(call.uavOutputs[slot])) return;
    for (const auto buffer : call.uavBuffers) {
        const auto* native = context.resources.Get(buffer);
        if (!native || native->GetBindlessUavIndex() == INVALID_BINDLESS_INDEX) return;
    }
    context.resources.Update(m_state.constants, &constants, sizeof(constants));
    call.dispatchX = (context.width + 7) / 8;
    call.dispatchY = (context.height + 7) / 8;
    context.renderer.Dispatch(call, context.resources);
    if (m_state.gameProfile) {
        ++m_state.game.frameSampleIndex;
        m_state.dispatchSucceeded = true;
    } else m_state.dispatchSucceeded = m_state.history.Commit(constants.samplesPerDispatch);
    context.rayPathPassActive = m_state.dispatchSucceeded;
}

} /// @note namespace fbzz::renderer
