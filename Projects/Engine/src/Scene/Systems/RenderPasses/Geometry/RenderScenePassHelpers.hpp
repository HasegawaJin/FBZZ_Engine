/// @file    RenderScenePassHelpers.hpp
/// @brief   抽出済みメッシュ入力からビュー別の描画コマンドを組み立てる。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#pragma once
#include "GeometryPasses.hpp"
#include <Graphics/Passes/Geometry/RenderSceneDraw.hpp>
#include <Engine/Renderer/RenderScene.hpp>
#include <Engine/Renderer/RenderVisibility.hpp>
#include <cassert>
#include <algorithm>

namespace fbzz::scene {

using renderer::ForwardDraw;

inline const renderer::RenderScene& SceneInput(const RenderPassContext& ctx)
{
    assert(ctx.renderScene && "RenderScene must be extracted after skinning and before drawing");
    return *ctx.renderScene;
}

inline bool MatchesView(const RenderPassContext& ctx, const renderer::RenderObject& object)
{
    return object.lodVisible && fbzz::Layer::Contains(ctx.cullingMask, object.layer);
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
    const float layerDistance = ctx.cullLayerDistances[object.layer & (kCullLayerCount - 1)];
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
    data.objectParams.z = object.gpuBones.current.IsValid() ? 1.0f : 0.0f;
    data.objectParams.w = static_cast<float>(object.gpuBones.currentOffset);
    return data;
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

}
