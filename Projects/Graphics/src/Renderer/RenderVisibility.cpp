/// @file    RenderVisibility.cpp
/// @brief   抽出済みの球とビューによる距離・極小・視錐台判定。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#include <Graphics/Renderer/RenderVisibility.hpp>
#include <Math/Frustum.hpp>

namespace fbzz::renderer {
namespace {

float ViewDistance(const RenderCullingView& view, const RenderCullingItem& item)
{
    const math::Vector3 toObject = item.center - view.position;
    return view.distanceSpherical ? toObject.Length() : math::Vector3::Dot(toObject, view.forward);
}

} /// @note namespace

VisibilityResult EvaluateVisibility(const RenderCullingView& view, const RenderCullingItem& item)
{
    if (item.radius <= 0.0f) return VisibilityResult::VISIBLE;
    const float distance = ViewDistance(view, item);
    /// @note 球の最近点で測り、大きい物体が距離境界をまたいでも丸ごと消さない。
    if (item.maxDrawDistance > 0.0f && distance - item.radius > item.maxDrawDistance)
        return VisibilityResult::DISTANCE_CULLED;

    if (view.smallObjectScreenHeight > 0.0f && view.projectionScaleY > 0.0f && distance > 0.0f) {
        /// @note 平行投影は距離で縮まない。奥行きによる除算は透視投影だけに適用する。
        const float screenHeight = view.orthographic
            ? item.radius * view.projectionScaleY
            : item.radius * view.projectionScaleY / distance;
        if (screenHeight < view.smallObjectScreenHeight)
            return VisibilityResult::SMALL_OBJECT_CULLED;
    }
    if (view.frustum && !view.frustum->IntersectsSphere(item.center, item.radius))
        return VisibilityResult::FRUSTUM_CULLED;
    return VisibilityResult::VISIBLE;
}

bool IsWithinDrawDistance(const RenderCullingView& view, const RenderCullingItem& item)
{
    if (item.maxDrawDistance <= 0.0f) return true;
    return ViewDistance(view, item) - item.radius <= item.maxDrawDistance;
}

} /// @note namespace fbzz::renderer
