/// @file    MeshTrailRenderPass.cpp
/// @brief   抽出済みメッシュ残像の描画。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#include <Graphics/Renderer/Commands/ShaderBindingSlots.hpp>
#include <Graphics/Passes/Geometry/Effects/MeshTrailRenderPass.hpp>
#include <Graphics/Scene/RenderScene.hpp>
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>

namespace fbzz::renderer {

std::string_view MeshTrailRenderPass::Name() const { return "MeshTrail"; }

void MeshTrailRenderPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    builder.ReadWrite("HDR").SetAutoTarget("HDR");
}

void MeshTrailRenderPass::Execute(PassResources&, RenderPassContext& ctx)
{
    auto& resources = ctx.resources;
    const auto& handles = ctx.handles;
    if (!ctx.renderScene) {
        return;
    }

    // 残像の頂点形式と片面・両面のどちらにも対応できるよう、共有資源を先に確認する。
    const bool shadersReady = handles.meshTrailShader.IsValid() && handles.skinnedMeshTrailShader.IsValid();
    if (!shadersReady) {
        return;
    }
    const bool pipelineStatesReady = handles.meshTrailPSO.IsValid() && handles.meshTrailDoubleSidedPSO.IsValid();
    if (!pipelineStatesReady) {
        return;
    }

    ctx.renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);
    for (const auto& input : ctx.renderScene->meshTrails) {
        if (input.layer >= 32 || (ctx.cullingMask & (1u << input.layer)) == 0) {
            continue;
        }
        ++ctx.statsTotalObjects;

        // 正の境界球がある残像だけにワールド空間のカリング余白を足す。
        const float paddedBoundsRadius = input.boundsRadius > 0.0f
            ? input.boundsRadius + std::max(ctx.cullingBoundsPadding, 0.0f)
            : 0.0f;
        if (IsViewOccluded(ctx, input.boundsCenter, paddedBoundsRadius)) {
            ++ctx.statsOcclusionCulled;
            continue;
        }

        // 共有のオブジェクト定数と、残像固有の色を描画直前に更新する。
        resources.Update(handles.objectCB, &input.object, sizeof(input.object));
        resources.Update(input.colorBuffer, &input.color, sizeof(input.color));

        DrawCall call;
        call.vertexBuffer = input.vertices;
        call.indexBuffer = input.indices;
        call.indexCount = input.indexCount;
        call.vertexCount = input.vertexCount;
        if (input.shader.IsValid()) {
            call.shader = input.shader;
        } else {
            call.shader = input.skinned ? handles.skinnedMeshTrailShader : handles.meshTrailShader;
        }
        call.pipelineState = input.doubleSided ? handles.meshTrailDoubleSidedPSO : handles.meshTrailPSO;
        call.layer = RenderLayer::TRANSPARENT_LAYER;
        call.constantBuffers[cb::CAMERA] = handles.frameCB;
        call.constantBuffers[cb::OBJECT] = handles.objectCB;
        call.constantBuffers[cb::MATERIAL] = input.material;
        call.constantBuffers[cb::MESHTRAIL] = input.colorBuffer;
        call.constantBuffers[cb::SKINNING] = input.skin;
        call.textures[srv::ALBEDO] = input.texture;
        SubmitCounted(ctx, call);
    }
}

} // namespace fbzz::renderer
