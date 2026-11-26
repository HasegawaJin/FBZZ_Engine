/// @file    RayGameReconstructionPass.cpp
/// @brief   Game 再構成の表面・輸送・ping-pong 履歴依存を記録する。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#include <Graphics/Renderer/Commands/ShaderBindingSlots.hpp>
#include <Graphics/Passes/RayTracing/RayGameReconstructionPass.hpp>
#include <Graphics/Pipeline/Passes/RenderPassContext.hpp>
#include <Graphics/Renderer/Commands/ComputeCall.hpp>
#include <Graphics/Renderer/Resources/Buffers/IStructuredBuffer.hpp>

namespace fbzz::renderer {

void RayGameReconstructionPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    using Purpose = RenderGraph::ResourceAccessPurpose;
    builder.Read("RayGameTransport", Purpose::SHADER_READ);
    builder.Read("RayGameSurface", Purpose::SHADER_READ);
    if (m_spatial) {
        builder.Read("RayGameHistoryNext", Purpose::SHADER_READ);
        builder.Write("RayPathResult", Purpose::UAV);
    } else {
        builder.Read("RayGameHistoryPrevious", Purpose::SHADER_READ);
        builder.Write("RayGameHistoryNext", Purpose::UAV);
    }
}

void RayGameReconstructionPass::Execute(PassResources& resources, RenderPassContext& context)
{
    auto& game = m_state.game;
    if (!m_state.dispatchSucceeded || !context.resources.Get(m_shader)
        || !context.resources.Get(game.reconstructionConstants)) {
        return;
    }
    if (m_spatial && !game.temporalSucceeded) {
        return;
    }
    auto constants = game.reconstructionData;
    constants.stage = m_spatial ? 1u : 0u;
    ComputeCall call;
    call.shader = m_shader;
    call.constantBuffers[cb::RAY_RECONSTRUCTION] = game.reconstructionConstants;
    if (m_spatial) {
        call.srvBuffers[srv::RAY_SPATIAL_HISTORY] = resources.StructuredBuffer("RayGameHistoryNext");
        call.srvBuffers[srv::RAY_SPATIAL_FRAME] = resources.StructuredBuffer("RayGameTransport");
        call.srvBuffers[srv::RAY_SPATIAL_SURFACE] = resources.StructuredBuffer("RayGameSurface");
        call.uavOutputs[uav::RAY_RESULT] = resources.Texture("RayPathResult");
    } else {
        call.srvBuffers[srv::RAY_RECON_FRAME] = resources.StructuredBuffer("RayGameTransport");
        call.srvBuffers[srv::RAY_RECON_SURFACE] = resources.StructuredBuffer("RayGameSurface");
        call.srvBuffers[srv::RAY_RECON_HISTORY] = resources.StructuredBuffer("RayGameHistoryPrevious");
        call.uavBuffers[StructuredUavIndex(uav::RAY_RECON_HISTORY)] = resources.StructuredBuffer("RayGameHistoryNext");
    }
    for (const uint32_t slot : {m_spatial ? srv::RAY_SPATIAL_HISTORY : srv::RAY_RECON_FRAME,
                               m_spatial ? srv::RAY_SPATIAL_FRAME : srv::RAY_RECON_SURFACE,
                               m_spatial ? srv::RAY_SPATIAL_SURFACE : srv::RAY_RECON_HISTORY}) {
        if (!context.resources.Get(call.srvBuffers[slot])) {
            return;
        }
    }
    if (m_spatial ? !context.resources.Get(call.uavOutputs[uav::RAY_RESULT]) : !context.resources.Get(call.uavBuffers[StructuredUavIndex(uav::RAY_RECON_HISTORY)])) {
        return;
    }
    context.resources.Update(game.reconstructionConstants, &constants, sizeof(constants));
    call.dispatchX = (context.width + 7) / 8;
    call.dispatchY = (context.height + 7) / 8;
    context.renderer.Dispatch(call, context.resources);
    if (!m_spatial) {
        game.temporalSucceeded = true;
    }
    if (m_spatial) {
        game.historyReadIndex ^= 1u;
        game.historyValid = true;
        game.reconstructed = true;
        game.previousCamera = context.camera;
        game.lastFrameStamp = context.frameStamp;
    }
}

} // namespace fbzz::renderer
