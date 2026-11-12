/// @file    ComputedSkinningDemand.cpp
/// @brief   色の可視判定と実 ShadowPass の共有判定から需要を合成する。
/// @author  Hasegawa Jin
/// @date    2026-10-16
#include <Graphics/Passes/Geometry/ComputedSkinningDemand.hpp>
#include <Graphics/Pipeline/RayTracingPipeline.hpp>
#include "RenderScenePassHelpers.hpp"
#include "ShadowCasterVisibility.hpp"
#include <algorithm>
#include <cmath>

namespace fbzz::renderer {

bool RequiresComputedSkinning(const RenderPassContext& context,
    const RenderObject& object, bool unknownConsumers)
{
    const bool ray = context.experimentalRayTracingEnabled
        && (context.settings.modeRequest.mode != RenderMode::RASTER
            || IsRayDebugView(context.settings.viewMode));
    if (unknownConsumers || ray || context.objectMaskEnabled
        || (context.selectionOutlineEnabled && object.selected)) return true;
    const auto bounds = RenderBounds(context, object);
    if (!std::isfinite(bounds.center.x) || !std::isfinite(bounds.center.y)
        || !std::isfinite(bounds.center.z) || !std::isfinite(bounds.radius)
        || bounds.radius <= 0.0f) return true;
    if (!MatchesView(context, object)) return false;

    /// @note Skinning 時点では材質を同期しないため、可視な色候補の route は狭めない。
    if (EvaluateVisibility(CullingView(context),
        { bounds.center, bounds.radius, DrawDistance(context, object) }) == VisibilityResult::VISIBLE) return true;
    if (!context.settings.shadowEnabled || !object.castShadows) return false;
    if (!IsWithinDrawDistance(CullingView(context),
        { bounds.center, bounds.radius, DrawDistance(context, object) })) return false;
    const bool directional = context.shadowStrength > 0.0f || context.settings.volumetricLight.enabled;
    if (directional && shadow_visibility_detail::ComputeCascadeMask(context,
            shadow_visibility_detail::DirectionalUpdateMask(context), bounds, true) != 0) return true;
    /// @note atlas や shader の実体未確認時も候補を残し、通常 shadow 準備を先取りしない。
    return shadow_visibility_detail::ComputePunctualMask(context, bounds, true) != 0;
}

}
