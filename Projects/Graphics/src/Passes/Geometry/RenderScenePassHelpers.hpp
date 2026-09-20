/// @file    RenderScenePassHelpers.hpp
/// @brief   抽出済みメッシュ入力からビュー別の描画コマンドを組み立てる。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#pragma once
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Renderer/RenderVisibility.hpp>
#include <cassert>
#include <algorithm>

namespace fbzz::renderer {

inline const renderer::RenderScene& SceneInput(const RenderPassContext& ctx)
{
    assert(ctx.renderScene && "RenderScene must be extracted after skinning and before drawing");
    return *ctx.renderScene;
}

inline bool MatchesView(const RenderPassContext& ctx, const renderer::RenderObject& object)
{
    return object.lodVisible && (object.layer < 32 && (ctx.cullingMask & (uint32_t{1} << object.layer)) != 0);
}

inline WorldBounds RenderBounds(const RenderPassContext& ctx, const renderer::RenderObject& object)
{
    return { object.boundsCenter, object.boundsRadius > 0.0f
        ? object.boundsRadius + (std::max)(ctx.cullingBoundsPadding, 0.0f) : 0.0f };
}

inline renderer::RenderCullingView CullingView(const RenderPassContext& ctx)
{
    return { ctx.camera.m_position, ctx.cullCameraForward,
        ctx.frustumCullingEnabled ? ctx.cameraFrustum : nullptr,
        ctx.cullProjScaleY, ctx.smallObjectScreenHeight, ctx.cullDistanceSpherical, ctx.cullOrthographic };
}

inline float DrawDistance(const RenderPassContext& ctx, const renderer::RenderObject& object)
{
    const float layerDistance = ctx.cullLayerDistances[object.layer & (32 - 1)];
    return ctx.hasLayerCullDistances && layerDistance > 0.0f ? layerDistance : ctx.cullMaxDistance;
}

inline bool IsRenderObjectVisible(RenderPassContext& ctx, const renderer::RenderObject& object)
{
    const auto bounds = RenderBounds(ctx, object);
    switch (renderer::EvaluateVisibility(CullingView(ctx),
        { bounds.center, bounds.radius, DrawDistance(ctx, object) })) {
    case renderer::VisibilityResult::DISTANCE_CULLED: ++ctx.statsDistanceCulled; return false;
    case renderer::VisibilityResult::SMALL_OBJECT_CULLED: ++ctx.statsSmallObjectCulled; return false;
    case renderer::VisibilityResult::FRUSTUM_CULLED: ++ctx.statsFrustumCulled; return false;
    case renderer::VisibilityResult::VISIBLE: return true;
    }
    return true;
}

inline PerObjectCB ObjectConstants(const renderer::RenderObject& object)
{
    PerObjectCB data{};
    data.world = object.world;
    data.worldInvTranspose = object.worldInvTranspose;
    data.objectParams.x = object.lodDither;
    return data;
}

inline auto MaterialPipeline(RenderPassContext& ctx, const renderer::RenderMaterial& material)
{
    return GetOrCreateMaterialPSO(ctx.resources, material.capabilities.blend,
        material.doubleSided, material.depthBias, material.depthBiasSlope, ctx.settings.IsWireframe());
}

inline renderer::DrawCall ForwardDraw(RenderPassContext& ctx, const renderer::RenderObject& object,
                                      const renderer::RenderMeshItem& item, bool bindIbl = true)
{
    const auto& material = item.forwardMaterial;
    const auto& h = ctx.handles;
    renderer::DrawCall dc;
    dc.vertexBuffer = object.skinned ? item.skinningVertexBuffer : item.vertexBuffer;
    dc.indexBuffer = item.indexBuffer;
    dc.indexCount = item.indexCount;
    dc.vertexCount = item.vertexCount;
    dc.shader = material.shader;
    dc.pipelineState = MaterialPipeline(ctx, item.material);
    dc.layer = material.capabilities.blend == renderer::BlendMode::OPAQUE_BLEND
        ? renderer::RenderLayer::OPAQUE_LAYER : renderer::RenderLayer::TRANSPARENT_LAYER;
    dc.constantBuffers[0] = h.frameCB;
    dc.constantBuffers[1] = h.objectCB;
    dc.constantBuffers[2] = material.paramsBuffer;
    dc.constantBuffers[3] = h.lightCB;
    dc.constantBuffers[4] = h.shadowCB;
    dc.constantBuffers[8] = h.advancedGraphicsCB;
    if (object.skinned) dc.constantBuffers[7] = object.skinningPalette;
    BindForwardShadingResources(dc, ctx);
    for (size_t i = 0; i < material.textures.size(); ++i) dc.textures[i] = material.textures[i];
    dc.textures[8] = ctx.resources.GetDepthTexture(ctx.Res().Target("ShadowMap"));
    if (bindIbl) {
        dc.textures[16] = h.iblIrradiance;
        dc.textures[17] = h.iblPrefilter;
        dc.textures[18] = h.iblBrdfLut;
    }
    return dc;
}

inline bool HasColorGeometry(const renderer::RenderMeshItem& item)
{
    return item.visible && item.vertexBuffer.IsValid() && item.indexBuffer.IsValid();
}

} /// @note namespace fbzz::renderer
