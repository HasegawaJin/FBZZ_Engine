/// @file    RayPathTracePass.cpp
/// @brief   型付き Path 資源と RAW UAV 履歴の依存を記録する。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#include <Graphics/Renderer/Commands/ShaderBindingSlots.hpp>
#include <Graphics/Passes/RayTracing/RayPathTracePass.hpp>
#include <Graphics/Passes/RayTracing/RayDebugPass.hpp>
#include <Graphics/Pipeline/Resources/RenderResources.hpp>
#include <Graphics/Renderer/Commands/ComputeCall.hpp>
#include <Graphics/Renderer/Resources/Buffers/IStructuredBuffer.hpp>

namespace fbzz::renderer {

bool MakeRayPathCameraConstants(const Camera& camera, RayPathTraceConstants& output)
{
    RayDebugConstants constants{};
    if (!MakeRayDebugCameraConstants(camera, constants)) {
        return false;
    }
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
        if (m_state.gpu.instanceCount) {
            builder.Read("RayGameMotionInstances", Purpose::SHADER_READ);
        }
    } else {
        builder.ReadWrite("RayPathHistory", Purpose::UAV);
        builder.Write("RayPathIds", Purpose::UAV);
    }
    for (const char* name : {"RayPathResult", "RayPathSurface", "RayPathMaterial", "RayPathGeometry"}) {
        builder.Write(name, Purpose::UAV);
    }
    builder.Read("RayPathEnvironment", Purpose::SHADER_READ);
    if (m_state.constantsData.emitterCount) {
        builder.Read("RayPathEmitters", Purpose::SHADER_READ);
    }
    if (m_state.constantsData.deltaLightCount) {
        builder.Read("RayPathDeltaLights", Purpose::SHADER_READ);
    }
    if (m_state.constantsData.shapeLightCount) {
        builder.Read("RayPathShapes", Purpose::SHADER_READ);
    }
    if (m_state.constantsData.environmentTableCount) {
        builder.Read("RayPathEnvironmentTable", Purpose::SHADER_READ);
    }
    if (m_state.gpu.instanceCount) {
        builder.Read("RaySceneTLAS", Purpose::TRACE_READ);
        builder.Read("RayHitRecords", Purpose::SHADER_READ);
        builder.Read("RaySurfaceMaterials", Purpose::SHADER_READ);
        for (size_t i = 0; i < m_state.gpu.readBuffers.size(); ++i) {
            builder.Read("RayReadBuffer" + std::to_string(i), Purpose::SHADER_READ);
        }
        for (size_t i = 0; i < m_state.gpu.readTextures.size(); ++i) {
            builder.Read("RayReadTexture" + std::to_string(i), Purpose::SHADER_READ);
        }
    }
}

void RayPathTracePass::Execute(PassResources& resources, RenderPassContext& context)
{
    m_state.dispatchSucceeded = false;
    context.rayPathPassActive = false;
    auto constants = m_state.constantsData;
    if (!MakeRayPathCameraConstants(context.camera, constants)
        || constants.samplesPerDispatch == 0 || constants.samplesPerDispatch > 64
        || constants.sampleBase > UINT32_MAX - 1u - constants.samplesPerDispatch) {
        return;
    }
    constants.width = context.width;
    constants.height = context.height;
    constants.instanceCount = m_state.gpu.instanceCount;
    ComputeCall call;
    call.shader = m_shader;
    call.constantBuffers[cb::RAY_PATH_TRACE] = m_state.constants;
    call.uavOutputs[uav::RAY_RESULT] = resources.Texture("RayPathResult");
    call.uavOutputs[uav::RAY_PATH_SURFACE] = resources.Texture("RayPathSurface");
    call.uavOutputs[uav::RAY_PATH_MATERIAL] = resources.Texture("RayPathMaterial");
    call.uavOutputs[uav::RAY_PATH_GEOMETRY] = resources.Texture("RayPathGeometry");
    if (m_state.gameProfile) {
        call.constantBuffers[cb::RAY_GAME_TRACE] = m_state.game.traceConstants;
        call.uavBuffers[StructuredUavIndex(uav::RAY_GAME_TRANSPORT)] = resources.StructuredBuffer("RayGameTransport");
        call.uavBuffers[StructuredUavIndex(uav::RAY_GAME_SURFACE)] = resources.StructuredBuffer("RayGameSurface");
        const auto target = resources.Target("GBuffer");
        call.srvInputs[srv::RAY_PATH_ALBEDO] = context.resources.GetColorTexture(target, 0);
        call.srvInputs[srv::RAY_PATH_NORMAL] = context.resources.GetColorTexture(target, 1);
        call.srvInputs[srv::RAY_PATH_DEPTH] = context.resources.GetDepthTexture(target);
        call.srvInputs[srv::RAY_PATH_EMISSION] = context.resources.GetColorTexture(target, 2);
        if (constants.instanceCount) {
            call.srvBuffers[srv::RAY_GAME_MOTION] = resources.StructuredBuffer("RayGameMotionInstances");
        }
        if (!context.resources.Get(call.constantBuffers[cb::RAY_GAME_TRACE])) {
            return;
        }
        for (const uint32_t slot : {srv::RAY_PATH_ALBEDO, srv::RAY_PATH_NORMAL, srv::RAY_PATH_DEPTH, srv::RAY_PATH_EMISSION}) {
            if (!context.resources.Get(call.srvInputs[slot])) {
                return;
            }
        }
        if (constants.instanceCount && !context.resources.Get(call.srvBuffers[srv::RAY_GAME_MOTION])) {
            return;
        }
        context.resources.Update(m_state.game.traceConstants, &m_state.game.traceData, sizeof(RayGameTraceConstants));
    } else {
        call.uavBuffers[StructuredUavIndex(uav::RAY_PATH_HISTORY)] = resources.StructuredBuffer("RayPathHistory");
        call.uavBuffers[StructuredUavIndex(uav::RAY_PATH_IDS)] = resources.StructuredBuffer("RayPathIds");
    }
    call.srvInputs[srv::RAY_PATH_ENVIRONMENT] = resources.Texture("RayPathEnvironment");
    if (constants.environmentMode == 2 && !context.resources.Get(call.srvInputs[srv::RAY_PATH_ENVIRONMENT])) {
        return;
    }
    if (constants.emitterCount) {
        call.srvBuffers[srv::RAY_EMITTERS] = resources.StructuredBuffer("RayPathEmitters");
    }
    if (constants.deltaLightCount) {
        call.srvBuffers[srv::RAY_PATH_LIGHTS] = resources.StructuredBuffer("RayPathDeltaLights");
    }
    if (constants.shapeLightCount) {
        call.srvBuffers[srv::RAY_PATH_SHAPES] = resources.StructuredBuffer("RayPathShapes");
    }
    if (constants.environmentTableCount) {
        call.srvBuffers[srv::RAY_PATH_ENV_TABLE] = resources.StructuredBuffer("RayPathEnvironmentTable");
    }
    if ((constants.emitterCount && !context.resources.Get(call.srvBuffers[srv::RAY_EMITTERS]))
        || (constants.deltaLightCount && !context.resources.Get(call.srvBuffers[srv::RAY_PATH_LIGHTS]))
        || (constants.shapeLightCount && !context.resources.Get(call.srvBuffers[srv::RAY_PATH_SHAPES]))
        || (constants.environmentTableCount && !context.resources.Get(call.srvBuffers[srv::RAY_PATH_ENV_TABLE]))) {
        return;
    }
    if (constants.instanceCount) {
        call.accelerationStructures[srv::RAY_SCENE] = resources.AccelerationStructure("RaySceneTLAS");
        call.srvBuffers[srv::RAY_HIT_RECORDS] = resources.StructuredBuffer("RayHitRecords");
        call.srvBuffers[srv::RAY_MATERIALS] = resources.StructuredBuffer("RaySurfaceMaterials");
        for (size_t i = 0; i < m_state.gpu.readBuffers.size(); ++i) {
            call.indirectReadBuffers.push_back(resources.Buffer("RayReadBuffer" + std::to_string(i)));
        }
        for (size_t i = 0; i < m_state.gpu.readTextures.size(); ++i) {
            call.indirectReadTextures.push_back(resources.Texture("RayReadTexture" + std::to_string(i)));
        }
    }
    if (!context.width || !context.height || !context.resources.Get(m_shader)
        || !context.resources.Get(m_state.constants)) {
        return;
    }
    for (const uint32_t slot : {uav::RAY_RESULT, uav::RAY_PATH_SURFACE, uav::RAY_PATH_MATERIAL, uav::RAY_PATH_GEOMETRY}) {
        if (!context.resources.Get(call.uavOutputs[slot])) {
            return;
        }
    }
    for (const auto buffer : call.uavBuffers) {
        const auto* native = context.resources.Get(buffer);
        if (!native || native->GetBindlessUavIndex() == INVALID_BINDLESS_INDEX) {
            return;
        }
    }
    context.resources.Update(m_state.constants, &constants, sizeof(constants));
    call.dispatchX = (context.width + 7) / 8;
    call.dispatchY = (context.height + 7) / 8;
    context.renderer.Dispatch(call, context.resources);
    if (m_state.gameProfile) {
        ++m_state.game.frameSampleIndex;
        m_state.dispatchSucceeded = true;
    } else {
        m_state.dispatchSucceeded = m_state.history.Commit(constants.samplesPerDispatch);
    }
    context.rayPathPassActive = m_state.dispatchSucceeded;
}

} // namespace fbzz::renderer
