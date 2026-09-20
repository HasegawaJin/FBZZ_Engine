/// @file    SkinningComputePass.cpp
/// @brief   シーン型を参照せず抽出済みスキニング要求を実行する。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>
namespace fbzz::renderer {
void ExecuteSkinningRequests(RenderPassContext& ctx)
{
    if (ctx.skinningExecuted) return;
    ctx.skinningExecuted = true;
    if (!ctx.handles.skinningComputeCS.IsValid() || ctx.skinningRequests.empty()) return;
    ctx.renderer.BeginComputeBatch();
    for (const auto& input : ctx.skinningRequests) {
        if (!input.sourceVertices.IsValid() || !input.bonePalette.IsValid() ||
            !input.outputVertices.IsValid() || input.vertexCount == 0) continue;
        const uint32_t constants[4] = {input.vertexCount, 0, 0, 0};
        ctx.resources.Update(ctx.handles.skinningCB, constants, sizeof(constants));
        ComputeCall call;
        call.shader = ctx.handles.skinningComputeCS;
        call.constantBuffers[0] = ctx.handles.skinningCB;
        call.srvBuffers[14] = input.sourceVertices;
        call.srvBuffers[15] = input.bonePalette;
        call.uavVertexBuffer = input.outputVertices;
        call.dispatchX = (input.vertexCount + 63u) / 64u;
        ctx.renderer.Dispatch(call, ctx.resources);
    }
    ctx.renderer.EndComputeBatch();
}
void SkinningComputePass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteSkinningRequests(ctx);
}
}
