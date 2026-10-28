/// @file    RenderScenePassHelpers.hpp
/// @brief   抽出済みメッシュ入力からビュー別の描画コマンドを組み立てる。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#pragma once
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Passes/Geometry/RenderSceneDraw.hpp>
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

inline bool IsRenderObjectVisible(RenderPassContext& ctx, const renderer::RenderObject& object, bool gpuFrustum = false)
{
    const auto bounds = RenderBounds(ctx, object);
    auto view = CullingView(ctx);
    bool gpuEstimateOutside = false;
    /// @note Keep CPU counters as estimates while the instance batch owns the actual GPU frustum decision.
    if (gpuFrustum && view.frustum) {
        gpuEstimateOutside = !view.frustum->IntersectsSphere(bounds.center, bounds.radius);
        view.frustum = nullptr;
    }
    switch (renderer::EvaluateVisibility(view,
        { bounds.center, bounds.radius, DrawDistance(ctx, object) })) {
    case renderer::VisibilityResult::DISTANCE_CULLED: ++ctx.statsDistanceCulled; return false;
    case renderer::VisibilityResult::SMALL_OBJECT_CULLED: ++ctx.statsSmallObjectCulled; return false;
    case renderer::VisibilityResult::FRUSTUM_CULLED: ++ctx.statsFrustumCulled; return false;
    case renderer::VisibilityResult::VISIBLE:
        if (gpuEstimateOutside) ++ctx.statsFrustumCulled;
        return true;
    }
    return true;
}

inline PerObjectCB ObjectConstants(const renderer::RenderObject& object)
{
    PerObjectCB data{};
    data.world = object.world;
    data.worldInvTranspose = object.worldInvTranspose;
    data.objectParams.x = object.lodDither;
    data.objectParams.z = object.gpuBones.current.IsValid() ? 1.0f : 0.0f;
    data.objectParams.w = static_cast<float>(object.gpuBones.currentOffset);
    return data;
}

/// @note t14/t15 belong to VS palette reads only; never attach them to particle or meshlet draws.
inline void BindGpuBonePalette(DrawCall& draw, const RenderObject& object)
{
    if (object.gpuBones.current.IsValid()) { draw.vsBuffers[0] = object.gpuBones.current; draw.constantBuffers[7] = {}; }
}

inline auto MaterialPipeline(RenderPassContext& ctx, const renderer::RenderMaterial& material)
{
    return GetOrCreateMaterialPSO(ctx.resources, material.capabilities.blend,
        material.doubleSided, material.depthBias, material.depthBiasSlope, ctx.settings.IsWireframe());
}

inline bool HasColorGeometry(const renderer::RenderMeshItem& item)
{
    return item.visible && item.vertexBuffer.IsValid() && item.indexBuffer.IsValid();
}

/// @note Borrow only this execution's resolved view resources; no registry or resource handle is retained across passes or views.
/// @note Long registry names create owning temporary strings; resolve them once before the per-object loop.
/// @see https://github.com/microsoft/STL/blob/main/stl/inc/xstring basic_string small storage and string_view construction.
inline DrawCall ForwardPassBindings(RenderPassContext& ctx, bool bindIbl = true)
{
    const auto& h = ctx.handles;
    DrawCall draw;
    draw.constantBuffers[0] = h.frameCB;
    draw.constantBuffers[1] = h.objectCB;
    draw.constantBuffers[3] = h.lightCB;
    draw.constantBuffers[4] = h.shadowCB;
    draw.constantBuffers[8] = h.advancedGraphicsCB;
    BindForwardShadingResources(draw, ctx);
    draw.textures[8] = ctx.resources.GetDepthTexture(ctx.Res().Target("ShadowMap"));
    if (bindIbl) {
        draw.textures[16] = h.iblIrradiance;
        draw.textures[17] = h.iblPrefilter;
        draw.textures[18] = h.iblBrdfLut;
    }
    return draw;
}

inline DrawCall ForwardDrawWithBindings(RenderPassContext& ctx, const RenderObject& object,
                                       const RenderMeshItem& item, const DrawCall& bindings)
{
    const auto& material = item.forwardMaterial;
    DrawCall draw = bindings;
    draw.vertexBuffer = object.skinned ? item.skinningVertexBuffer : item.vertexBuffer;
    draw.indexBuffer = item.indexBuffer;
    draw.indexCount = item.indexCount;
    draw.vertexCount = item.vertexCount;
    draw.shader = material.shader;
    draw.pipelineState = MaterialPipeline(ctx, item.material);
    draw.layer = material.capabilities.blend == BlendMode::OPAQUE_BLEND
        ? RenderLayer::OPAQUE_LAYER : RenderLayer::TRANSPARENT_LAYER;
    draw.constantBuffers[2] = material.paramsBuffer;
    if (object.skinned) { draw.constantBuffers[7] = object.skinningPalette; BindGpuBonePalette(draw, object); }
    for (size_t i = 0; i < material.textures.size(); ++i) draw.textures[i] = material.textures[i];
    return draw;
}

} /// @note namespace fbzz::renderer
