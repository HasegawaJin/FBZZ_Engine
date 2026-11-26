/// @file    RayReflectionReconstructionPass.cpp
/// @brief   反射の静止履歴・空間境界と成功時だけの出力公開を記録する。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#include <Graphics/Renderer/Commands/ShaderBindingSlots.hpp>
#include <Graphics/Passes/RayTracing/RayReflectionReconstructionPass.hpp>
#include <Graphics/Pipeline/Passes/RenderPassContext.hpp>
#include <Graphics/Renderer/Commands/ComputeCall.hpp>
#include <Graphics/Renderer/IRenderer.hpp>

namespace fbzz::renderer {
void RayReflectionReconstructionPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    using Purpose = RenderGraph::ResourceAccessPurpose;
    builder.Read("RayReflectionRaw", Purpose::SHADER_READ);
    builder.Read("RayReflectionSurface", Purpose::SHADER_READ);
    if (m_state.reconstruction.constantsData.resolutionDivisor == 2) {
        builder.Read("RayReflectionHalfRaw", Purpose::SHADER_READ);
    }
    if (m_state.reconstruction.constantsData.hasGBuffer) {
        builder.Read("GBuffer", Purpose::SHADER_READ);
    }
    if (m_spatial) {
        builder.Read("RayReflectionHistory", Purpose::SHADER_READ);
        builder.Write("RayReflectionResult", Purpose::UAV);
    } else {
        builder.Read("RayReflectionSurfacePrevious", Purpose::SHADER_READ);
        builder.Read("RayReflectionHistoryPrevious", Purpose::SHADER_READ);
        builder.Write("RayReflectionHistory", Purpose::UAV);
    }
}

void RayReflectionReconstructionPass::Execute(PassResources& resources, RenderPassContext& context)
{
    auto& state = m_state.reconstruction;
    const auto reject = [&]() {
        state.historyValid = false;
        state.temporalSucceeded = false;
        context.handles.rayReflectionResult = m_state.output;
    };
    if (!state.prepared || !state.rawSucceeded || !context.rayReflectionPassActive
        || !context.resources.Get(m_shader) || !context.resources.Get(state.constants)
        || (m_spatial && !state.temporalSucceeded)) { reject(); return; }
    auto constants = state.constantsData;
    if (constants.width != context.width || constants.height != context.height) { reject(); return; }
    constants.stage = m_spatial ? 1u : 0u;
    ComputeCall call;
    call.shader = m_shader;
    call.constantBuffers[cb::RAY_REFLECTION_RECONSTRUCTION] = state.constants;
    call.srvInputs[srv::REFLECTION_RAW] = resources.Texture("RayReflectionRaw");
    if (constants.resolutionDivisor == 2) {
        call.srvInputs[srv::REFLECTION_RAW_HALF] = resources.Texture("RayReflectionHalfRaw");
        const auto* halfRaw = context.resources.Get(call.srvInputs[srv::REFLECTION_RAW_HALF]);
        if (!halfRaw || !constants.hasGBuffer
            || constants.traceWidth != context.width / 2 + context.width % 2
            || constants.traceHeight != context.height / 2 + context.height % 2
            || halfRaw->GetWidth() != constants.traceWidth || halfRaw->GetHeight() != constants.traceHeight
            || halfRaw->GetBindlessIndex() == INVALID_BINDLESS_INDEX) { reject(); return; }
    }
    const auto surfaceSlot = m_spatial ? srv::REFLECTION_SPATIAL_HISTORY : srv::REFLECTION_RECON_SURFACE;
    const auto previousSlot = m_spatial ? srv::REFLECTION_SPATIAL_SURFACE : srv::REFLECTION_RECON_PREVIOUS_SURFACE;
    call.srvBuffers[surfaceSlot] = resources.StructuredBuffer(m_spatial ? "RayReflectionHistory" : "RayReflectionSurface");
    call.srvBuffers[previousSlot] = resources.StructuredBuffer(m_spatial ? "RayReflectionSurface" : "RayReflectionSurfacePrevious");
    if (constants.hasGBuffer) {
        call.srvInputs[srv::REFLECTION_RECON_ALBEDO] = context.resources.GetColorTexture(resources.Target("GBuffer"), 0);
        call.srvInputs[srv::REFLECTION_RECON_NORMAL] = context.resources.GetColorTexture(resources.Target("GBuffer"), 1);
        if (!context.resources.Get(call.srvInputs[srv::REFLECTION_RECON_ALBEDO]) || !context.resources.Get(call.srvInputs[srv::REFLECTION_RECON_NORMAL])) { reject(); return; }
    }
    if (m_spatial) {
        call.uavOutputs[uav::RAY_RESULT] = resources.Texture("RayReflectionResult");
    } else {
        call.srvBuffers[srv::REFLECTION_RECON_HISTORY] = resources.StructuredBuffer("RayReflectionHistoryPrevious");
        call.uavBuffers[StructuredUavIndex(uav::REFLECTION_RECON_HISTORY)] = resources.StructuredBuffer("RayReflectionHistory");
    }
    if (!context.resources.Get(call.srvInputs[srv::REFLECTION_RAW]) || !context.resources.Get(call.srvBuffers[surfaceSlot])
        || !context.resources.Get(call.srvBuffers[previousSlot])
        || (m_spatial ? !context.resources.Get(call.uavOutputs[uav::RAY_RESULT])
            : !context.resources.Get(call.uavBuffers[StructuredUavIndex(uav::REFLECTION_RECON_HISTORY)]) || !context.resources.Get(call.srvBuffers[srv::REFLECTION_RECON_HISTORY]))) {
        reject(); return;
    }
    // Distinct previous/current history resources prevent inter-pixel reprojection races; spatial reads only after backend UAV ordering.
    // Update supplies an immutable submission snapshot: stage1 cannot mutate stage0's in-flight constants.
    context.resources.Update(state.constants, &constants, sizeof(constants));
    call.dispatchX = (context.width + 7) / 8;
    call.dispatchY = (context.height + 7) / 8;
    if (!context.renderer.TryDispatch(call, context.resources)) { reject(); return; }
    if (!m_spatial) {
        state.temporalSucceeded = true;
    }
    else {
        context.handles.rayReflectionResult = state.output;
        state.surfaceReadIndex ^= 1u;
        state.historyValid = true;
        state.lastFrameStamp = context.frameStamp;
        state.previousJitterX = constants.currentJitterX;
        state.previousJitterY = constants.currentJitterY;
        state.committedCamera = constants;
    }
}
} // namespace fbzz::renderer
