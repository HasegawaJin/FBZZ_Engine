/// @file    SkinningComputePass.cpp
/// @brief   シーン型を参照せず抽出済みスキニング要求を実行する。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Graphics/Renderer/IBuffer.hpp>
#include <Graphics/Renderer/IStructuredBuffer.hpp>
#include <Math/Matrix4.hpp>
namespace fbzz::renderer {
void ExecuteSkinningRequests(RenderPassContext& ctx)
{
    if (ctx.skinningExecuted) return;
    ctx.skinningExecuted = true;
    if (!ctx.resources.Get(ctx.handles.skinningComputeCS) || !ctx.resources.Get(ctx.handles.skinningCB)
        || ctx.skinningRequests.empty()) return;
    ctx.renderer.BeginComputeBatch();
    for (const auto& input : ctx.skinningRequests) {
        if (!input.sourceVertices.IsValid() || !input.bonePalette.IsValid() ||
            !input.outputVertices.IsValid() || input.vertexCount == 0) continue;
        const auto* source = ctx.resources.Get(input.sourceVertices);
        const auto* palette = ctx.resources.Get(input.bonePalette);
        const auto* output = ctx.resources.Get(input.outputVertices);
        /// @note Engine の公開前条件と揃え、容量不足の要求では Dispatch と GPU 内容版更新の両方を行わない。
        if (!source || !palette || !output
            || source->GetStride() != sizeof(SkinnedVertex)
            || source->GetElementCount() < input.vertexCount
            || source->GetSize() / sizeof(SkinnedVertex) < input.vertexCount
            || palette->GetStride() != sizeof(math::Matrix4)
            || palette->GetElementCount() == 0
            || palette->GetSize() / sizeof(math::Matrix4) < palette->GetElementCount()
            || output->GetStride() != sizeof(Vertex)
            || output->GetSize() / sizeof(Vertex) < input.vertexCount
            || output->GetBindlessUavIndex() == INVALID_BINDLESS_INDEX) continue;
        const uint32_t constants[4] = {input.vertexCount, 0, 0, 0};
        ctx.resources.Update(ctx.handles.skinningCB, constants, sizeof(constants));
        ComputeCall call;
        call.shader = ctx.handles.skinningComputeCS;
        call.constantBuffers[0] = ctx.handles.skinningCB;
        call.srvBuffers[14] = input.sourceVertices;
        call.srvBuffers[15] = input.bonePalette;
        call.uavVertexBuffer = input.outputVertices;
        call.dispatchX = (input.vertexCount - 1u) / 64u + 1u;
        ctx.renderer.Dispatch(call, ctx.resources);
    }
    ctx.renderer.EndComputeBatch();
}
void SkinningComputePass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteSkinningRequests(ctx);
}
}
