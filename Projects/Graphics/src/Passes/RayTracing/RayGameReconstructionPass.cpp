/// @file    RayGameReconstructionPass.cpp
/// @brief   Game 再構成の表面・輸送・ping-pong 履歴依存を記録する。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#include <Graphics/Passes/RayTracing/RayGameReconstructionPass.hpp>
#include <Graphics/Pipeline/RenderPassContext.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Graphics/Renderer/IStructuredBuffer.hpp>

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
        || !context.resources.Get(game.reconstructionConstants)) return;
    if (m_spatial && !game.temporalSucceeded) return;
    auto constants = game.reconstructionData;
    constants.stage = m_spatial ? 1u : 0u;
    ComputeCall call;
    call.shader = m_shader;
    call.constantBuffers[0] = game.reconstructionConstants;
    if (m_spatial) {
        call.srvBuffers[14] = resources.StructuredBuffer("RayGameHistoryNext");
        call.srvBuffers[15] = resources.StructuredBuffer("RayGameTransport");
        call.srvBuffers[29] = resources.StructuredBuffer("RayGameSurface");
        call.uavOutputs[0] = resources.Texture("RayPathResult");
    } else {
        call.srvBuffers[14] = resources.StructuredBuffer("RayGameTransport");
        call.srvBuffers[15] = resources.StructuredBuffer("RayGameSurface");
        call.srvBuffers[29] = resources.StructuredBuffer("RayGameHistoryPrevious");
        call.uavBuffers[0] = resources.StructuredBuffer("RayGameHistoryNext");
    }
    for (const uint32_t slot : {14u, 15u, 29u})
        if (!context.resources.Get(call.srvBuffers[slot])) return;
    if (m_spatial ? !context.resources.Get(call.uavOutputs[0]) : !context.resources.Get(call.uavBuffers[0])) return;
    context.resources.Update(game.reconstructionConstants, &constants, sizeof(constants));
    call.dispatchX = (context.width + 7) / 8;
    call.dispatchY = (context.height + 7) / 8;
    context.renderer.Dispatch(call, context.resources);
    if (!m_spatial) game.temporalSucceeded = true;
    if (m_spatial) {
        game.historyReadIndex ^= 1u;
        game.historyValid = true;
        game.reconstructed = true;
        game.previousCamera = context.camera;
        game.lastFrameStamp = context.frameStamp;
    }
}

} /// @note namespace fbzz::renderer
