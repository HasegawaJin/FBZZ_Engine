/// @file    RayReflectionReconstructionPass.cpp
/// @brief   反射の静止履歴・空間境界と成功時だけの出力公開を記録する。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#include <Graphics/Passes/RayTracing/RayReflectionReconstructionPass.hpp>
#include <Graphics/Pipeline/RenderPassContext.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Graphics/Renderer/IRenderer.hpp>

namespace fbzz::renderer {
void RayReflectionReconstructionPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    using Purpose = RenderGraph::ResourceAccessPurpose;
    builder.Read("RayReflectionRaw", Purpose::SHADER_READ);
    builder.Read("RayReflectionSurface", Purpose::SHADER_READ);
    if (m_spatial) {
        builder.Read("RayReflectionHistory", Purpose::SHADER_READ);
        if (m_state.reconstruction.constantsData.hasGBuffer) builder.Read("GBuffer", Purpose::SHADER_READ);
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
    call.constantBuffers[0] = state.constants;
    call.srvInputs[5] = resources.Texture("RayReflectionRaw");
    call.srvBuffers[14] = resources.StructuredBuffer(m_spatial ? "RayReflectionHistory" : "RayReflectionSurface");
    call.srvBuffers[15] = resources.StructuredBuffer(m_spatial ? "RayReflectionSurface" : "RayReflectionSurfacePrevious");
    if (m_spatial) {
        call.uavOutputs[0] = resources.Texture("RayReflectionResult");
        if (constants.hasGBuffer) {
            call.srvInputs[16] = context.resources.GetColorTexture(resources.Target("GBuffer"), 0);
            call.srvInputs[17] = context.resources.GetColorTexture(resources.Target("GBuffer"), 1);
            if (!context.resources.Get(call.srvInputs[16]) || !context.resources.Get(call.srvInputs[17])) { reject(); return; }
        }
    } else {
        call.srvBuffers[18] = resources.StructuredBuffer("RayReflectionHistoryPrevious");
        call.uavBuffers[0] = resources.StructuredBuffer("RayReflectionHistory");
    }
    if (!context.resources.Get(call.srvInputs[5]) || !context.resources.Get(call.srvBuffers[14])
        || !context.resources.Get(call.srvBuffers[15])
        || (m_spatial ? !context.resources.Get(call.uavOutputs[0])
            : !context.resources.Get(call.uavBuffers[0]) || !context.resources.Get(call.srvBuffers[18]))) {
        reject(); return;
    }
    /// @note Distinct previous/current history resources prevent inter-pixel reprojection races; spatial reads only after backend UAV ordering.
    /// @note Update supplies an immutable submission snapshot: stage1 cannot mutate stage0's in-flight constants.
    context.resources.Update(state.constants, &constants, sizeof(constants));
    call.dispatchX = (context.width + 7) / 8;
    call.dispatchY = (context.height + 7) / 8;
    if (!context.renderer.TryDispatch(call, context.resources)) { reject(); return; }
    if (!m_spatial) state.temporalSucceeded = true;
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
} /// @note namespace fbzz::renderer
