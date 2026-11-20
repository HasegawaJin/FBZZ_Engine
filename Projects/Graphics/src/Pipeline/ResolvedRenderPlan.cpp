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

RenderPlanReason ResolveHybridEffects(ResolvedRenderPlan& plan, const RenderModeRequest& request,
    const SceneRayCoverage& coverage, const RenderAvailability& availability, RenderPlanReason prerequisite)
{
    if (!request.rayShadow && !request.rayReflection && !request.rayDiffuseGi)
        return RenderPlanReason::NO_RAY_EFFECT_REQUESTED;

    plan.shadow = ResolveRayEffect(request.rayShadow, coverage.shadow, availability.shadowPipelineReady, prerequisite);
    plan.reflection = ResolveRayEffect(request.rayReflection, coverage.reflection, availability.reflectionPipelineReady, prerequisite);
    plan.diffuseGi = ResolveRayEffect(request.rayDiffuseGi, coverage.diffuseGi, availability.diffuseGiPipelineReady, prerequisite);
    if (plan.shadow.enabled || plan.reflection.enabled || plan.diffuseGi.enabled)
        return RenderPlanReason::NONE;

    /// @note ビュー全体の復帰理由は、要求した効果のうちシャドウ、反射、拡散 GI の順で選ぶ。
    if (request.rayShadow) return plan.shadow.fallbackReason;
    if (request.rayReflection) return plan.reflection.fallbackReason;
    return plan.diffuseGi.fallbackReason;
}

RenderPlanReason ResolvePathVisibility(ResolvedRenderPlan& plan, const SceneRayCoverage& coverage,
    const RenderAvailability& availability, RenderPlanReason prerequisite)
{
    const bool gamePath = plan.pathProfile == PathTracingProfile::GAME;
    if (!gamePath && plan.pathProfile != PathTracingProfile::REFERENCE) return RenderPlanReason::INVALID_REQUEST;

    const auto effect = ResolveRayEffect(true, coverage.path, availability.pathPipelineReady, prerequisite);
    if (!effect.enabled) return effect.fallbackReason;
    if (gamePath && !availability.rasterSurfaceReady) return RenderPlanReason::RASTER_SURFACE_UNAVAILABLE;

    if (gamePath)
        plan.rasterPlan.path = OpaqueRenderPath::DEFERRED;
    else
        plan.primaryVisibility = PrimaryVisibility::CAMERA_RAY;
    return RenderPlanReason::NONE;
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
    case RenderPlanReason::DEVELOPER_MODE_REQUIRED: return "RT / Path Tracing の実験機能には DeveloperMode が必要です";
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

    const bool hybrid = request.mode == RenderMode::HYBRID;
    if (!hybrid && request.mode != RenderMode::PATH_TRACING) {
        const auto reason = request.mode == RenderMode::RASTER ? RenderPlanReason::NONE : RenderPlanReason::INVALID_REQUEST;
        ResolveRasterFallback(plan, availability, reason);
        return plan;
    }

    if (!availability.experimentalRayTracingEnabled) {
        const RayEffectPlan disabled{ false, RenderPlanReason::DEVELOPER_MODE_REQUIRED };
        if (hybrid) {
            if (request.rayShadow) plan.shadow = disabled;
            if (request.rayReflection) plan.reflection = disabled;
            if (request.rayDiffuseGi) plan.diffuseGi = disabled;
        }
        ResolveRasterFallback(plan, availability, RenderPlanReason::DEVELOPER_MODE_REQUIRED);
        return plan;
    }

    if (hybrid && !availability.rasterPipelineReady) {
        plan.failureReason = RenderPlanReason::RASTER_PIPELINE_UNAVAILABLE;
        return plan;
    }

    const auto prerequisite = ResolveRayPrerequisite(capabilities, availability);
    const auto reason = hybrid ? ResolveHybridEffects(plan, request, coverage, availability, prerequisite)
        : ResolvePathVisibility(plan, coverage, availability, prerequisite);
    if (reason == RenderPlanReason::NONE) {
        plan.effectiveMode = request.mode;
        plan.rayExecution = RayExecution::INLINE_RAY_QUERY;
        plan.failureReason = RenderPlanReason::NONE;
    } else {
        ResolveRasterFallback(plan, availability, reason);
    }
    return plan;
}

} /// @note namespace fbzz::renderer
