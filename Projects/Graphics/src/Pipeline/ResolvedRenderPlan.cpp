/// @file    ResolvedRenderPlan.cpp
/// @brief   任意の RT 効果と Path の復帰を副作用なしで決定する。
/// @author  Hasegawa Jin
/// @date    2026-09-30
#include <Graphics/Pipeline/ResolvedRenderPlan.hpp>
#include <Graphics/Renderer/RenderSettings.hpp>

namespace fbzz::renderer {
namespace {

RenderPlanReason ResolveRayPrerequisite(
    const GraphicsCapabilities& capabilities, const RenderAvailability& availability)
{
    if (!capabilities.inlineRayQuery)
        return RenderPlanReason::INLINE_RAY_QUERY_UNAVAILABLE;
    if (!capabilities.bindless)
        return RenderPlanReason::BINDLESS_UNAVAILABLE;
    if (!availability.raySceneReady)
        return RenderPlanReason::RAY_SCENE_UNAVAILABLE;
    return RenderPlanReason::NONE;
}

RayEffectPlan ResolveRayEffect(bool requested, bool covered, bool pipelineReady,
                               RenderPlanReason prerequisite)
{
    if (!requested)
        return {};
    if (prerequisite != RenderPlanReason::NONE)
        return { false, prerequisite };
    if (!covered)
        return { false, RenderPlanReason::RAY_COVERAGE_INCOMPLETE };
    if (!pipelineReady)
        return { false, RenderPlanReason::RAY_PIPELINE_UNAVAILABLE };
    return { true, RenderPlanReason::NONE };
}

void ResolveRasterFallback(ResolvedRenderPlan& plan, const RenderAvailability& availability,
                           RenderPlanReason reason)
{
    plan.fallbackReason = reason;
    plan.failureReason = availability.rasterPipelineReady
        ? RenderPlanReason::NONE : RenderPlanReason::RASTER_PIPELINE_UNAVAILABLE;
}

} /// @note namespace

const char* DescribeRenderPlanReason(RenderPlanReason reason)
{
    switch (reason) {
    case RenderPlanReason::NONE: return "";
    case RenderPlanReason::INVALID_REQUEST: return "描画モードまたは Path のプロファイルが不正です";
    case RenderPlanReason::NO_RAY_EFFECT_REQUESTED: return "RT 効果が選択されていません";
    case RenderPlanReason::INLINE_RAY_QUERY_UNAVAILABLE: return "GPU が Inline RayQuery に対応していません";
    case RenderPlanReason::BINDLESS_UNAVAILABLE: return "GPU が必要な Bindless 機能に対応していません";
    case RenderPlanReason::RAY_SCENE_UNAVAILABLE: return "レイトレーシング用のシーンを準備できていません";
    case RenderPlanReason::RAY_COVERAGE_INCOMPLETE: return "必要な形状・材質・光源に未対応の表現があります";
    case RenderPlanReason::RAY_PIPELINE_UNAVAILABLE: return "要求された RT / Path の描画パスを準備できていません";
    case RenderPlanReason::RASTER_SURFACE_UNAVAILABLE: return "Game Path に必要な Deferred の表面入力がありません";
    case RenderPlanReason::RASTER_PIPELINE_UNAVAILABLE: return "Raster の描画経路を準備できていません";
    case RenderPlanReason::REQUIRED_OUTPUT_UNAVAILABLE: return "出力先を準備できていません";
    }
    return "不明な描画構成の診断です";
}

ResolvedRenderPlan ResolveRenderPlan(
    const RenderSettings& settings, const RenderModeRequest& request,
    const GraphicsCapabilities& capabilities, const SceneRayCoverage& coverage,
    const RenderAvailability& availability)
{
    ResolvedRenderPlan plan;
    plan.requestedMode = request.mode;
    plan.pathProfile = request.pathProfile;
    plan.rasterPlan = ResolveOpaqueRenderPlan(settings, availability.opaque);
    plan.clusteredLighting = settings.UsesClusteredLighting() && availability.clusteredLightingReady;
    if (!availability.outputsReady)
        return plan;

    if (request.mode == RenderMode::RASTER) {
        ResolveRasterFallback(plan, availability, RenderPlanReason::NONE);
        return plan;
    }
    if (request.mode != RenderMode::HYBRID && request.mode != RenderMode::PATH_TRACING) {
        ResolveRasterFallback(plan, availability, RenderPlanReason::INVALID_REQUEST);
        return plan;
    }

    if (request.mode == RenderMode::HYBRID) {
        if (!availability.rasterPipelineReady) {
            plan.failureReason = RenderPlanReason::RASTER_PIPELINE_UNAVAILABLE;
            return plan;
        }
        if (!request.rayShadow && !request.rayReflection && !request.rayDiffuseGi) {
            ResolveRasterFallback(plan, availability, RenderPlanReason::NO_RAY_EFFECT_REQUESTED);
            return plan;
        }
        const auto prerequisite = ResolveRayPrerequisite(capabilities, availability);
        plan.shadow = ResolveRayEffect(request.rayShadow, coverage.shadow,
            availability.shadowPipelineReady, prerequisite);
        plan.reflection = ResolveRayEffect(request.rayReflection, coverage.reflection,
            availability.reflectionPipelineReady, prerequisite);
        plan.diffuseGi = ResolveRayEffect(request.rayDiffuseGi, coverage.diffuseGi,
            availability.diffuseGiPipelineReady, prerequisite);
        if (plan.shadow.enabled || plan.reflection.enabled || plan.diffuseGi.enabled) {
            plan.effectiveMode = RenderMode::HYBRID;
            plan.rayExecution = RayExecution::INLINE_RAY_QUERY;
            plan.failureReason = RenderPlanReason::NONE;
        } else {
            const auto reason = request.rayShadow ? plan.shadow.fallbackReason
                : request.rayReflection ? plan.reflection.fallbackReason : plan.diffuseGi.fallbackReason;
            ResolveRasterFallback(plan, availability, reason);
        }
        return plan;
    }

    if (request.pathProfile != PathTracingProfile::REFERENCE
        && request.pathProfile != PathTracingProfile::GAME) {
        ResolveRasterFallback(plan, availability, RenderPlanReason::INVALID_REQUEST);
        return plan;
    }
    auto reason = ResolveRayPrerequisite(capabilities, availability);
    if (reason == RenderPlanReason::NONE && !coverage.path)
        reason = RenderPlanReason::RAY_COVERAGE_INCOMPLETE;
    if (reason == RenderPlanReason::NONE && !availability.pathPipelineReady)
        reason = RenderPlanReason::RAY_PIPELINE_UNAVAILABLE;
    if (reason == RenderPlanReason::NONE && request.pathProfile == PathTracingProfile::GAME
        && !availability.rasterSurfaceReady)
        reason = RenderPlanReason::RASTER_SURFACE_UNAVAILABLE;
    if (reason != RenderPlanReason::NONE) {
        ResolveRasterFallback(plan, availability, reason);
        return plan;
    }

    plan.effectiveMode = RenderMode::PATH_TRACING;
    plan.primaryVisibility = request.pathProfile == PathTracingProfile::REFERENCE
        ? PrimaryVisibility::CAMERA_RAY : PrimaryVisibility::RASTER;
    if (request.pathProfile == PathTracingProfile::GAME)
        plan.rasterPlan.path = OpaqueRenderPath::DEFERRED;
    plan.rayExecution = RayExecution::INLINE_RAY_QUERY;
    plan.failureReason = RenderPlanReason::NONE;
    return plan;
}

}
