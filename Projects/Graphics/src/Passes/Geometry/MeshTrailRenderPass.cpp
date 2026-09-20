/// @file    MeshTrailRenderPass.cpp
/// @brief   抽出済みメッシュ残像の描画。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#include <Graphics/Passes/Geometry/MeshTrailRenderPass.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
namespace fbzz::renderer {
std::string_view MeshTrailRenderPass::Name() const { return "MeshTrail"; }

void MeshTrailRenderPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    builder.ReadWrite("HDR").SetAutoTarget("HDR");
}

void MeshTrailRenderPass::Execute(PassResources&, RenderPassContext& ctx) {
    auto& resources = ctx.resources;
    const auto& h = ctx.handles;
    if (!ctx.renderScene || !h.meshTrailShader.IsValid() || !h.skinnedMeshTrailShader.IsValid()
        || !h.meshTrailPSO.IsValid() || !h.meshTrailDoubleSidedPSO.IsValid()) return;
    ctx.renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);
    for (const auto& input : ctx.renderScene->meshTrails) {
        if (input.layer >= 32 || (ctx.cullingMask & (1u << input.layer)) == 0) continue;
        resources.Update(h.objectCB, &input.object, sizeof(input.object));
        resources.Update(input.colorBuffer, &input.color, sizeof(input.color));
        DrawCall call;
        call.vertexBuffer = input.vertices; call.indexBuffer = input.indices;
        call.indexCount = input.indexCount; call.vertexCount = input.vertexCount;
        call.shader = input.shader.IsValid() ? input.shader : input.skinned ? h.skinnedMeshTrailShader : h.meshTrailShader;
        call.pipelineState = input.doubleSided ? h.meshTrailDoubleSidedPSO : h.meshTrailPSO;
        call.layer = RenderLayer::TRANSPARENT_LAYER;
        call.constantBuffers[0] = h.frameCB; call.constantBuffers[1] = h.objectCB;
        call.constantBuffers[2] = input.material; call.constantBuffers[6] = input.colorBuffer;
        call.constantBuffers[7] = input.skin; call.textures[0] = input.texture;
        SubmitCounted(ctx, call);
    }
}
}
