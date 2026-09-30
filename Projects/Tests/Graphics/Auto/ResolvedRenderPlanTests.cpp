/// @file    ResolvedRenderPlanTests.cpp
/// @brief   GPU 能力・表現被覆・資源失敗によるモードと効果の復帰を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-30
#include <TestKit/TestKit.hpp>
#include <Graphics/Pipeline/ResolvedRenderPlan.hpp>
#include <Graphics/Renderer/RenderSettings.hpp>

namespace fbzz::tests {
namespace {

class ResolvedRenderPlanTest : public testkit::Fixture {};

renderer::GraphicsCapabilities RayCapabilities()
{
    return { true, true, true };
}

renderer::SceneRayCoverage CoveredScene()
{
    return { true, true, true, true };
}

renderer::RenderAvailability PreparedResources()
{
    renderer::RenderAvailability availability;
    availability.outputsReady = true;
    availability.rasterPipelineReady = true;
    availability.opaque = { true, true, true };
    availability.clusteredLightingReady = true;
    availability.raySceneReady = true;
    availability.shadowPipelineReady = true;
    availability.reflectionPipelineReady = true;
    availability.diffuseGiPipelineReady = true;
    availability.pathPipelineReady = true;
    availability.rasterSurfaceReady = true;
    return availability;
}

renderer::RenderModeRequest HybridRequest()
{
    renderer::RenderModeRequest request;
    request.mode = renderer::RenderMode::HYBRID;
    request.rayShadow = true;
    request.rayReflection = true;
    request.rayDiffuseGi = true;
    return request;
}

renderer::RenderModeRequest PathRequest(renderer::PathTracingProfile profile)
{
    renderer::RenderModeRequest request;
    request.mode = renderer::RenderMode::PATH_TRACING;
    request.pathProfile = profile;
    return request;
}

TEST_F(ResolvedRenderPlanTest, UnpreparedOutputNeverPublishesAValidPlan)
{
    renderer::RenderSettings settings;
    auto availability = PreparedResources();
    availability.outputsReady = false;
    for (const auto request : { renderer::RenderModeRequest{}, HybridRequest(),
                               PathRequest(renderer::PathTracingProfile::REFERENCE) }) {
        const auto plan = renderer::ResolveRenderPlan(settings, request, RayCapabilities(),
            CoveredScene(), availability);
        EXPECT_FALSE(plan.IsValid());
        EXPECT_EQ(plan.failureReason, renderer::RenderPlanReason::REQUIRED_OUTPUT_UNAVAILABLE);
        EXPECT_FALSE(plan.NeedsRayScene());
        EXPECT_FALSE(plan.shadow.enabled);
    }
}

TEST_F(ResolvedRenderPlanTest, RasterDoesNotDemandRayResourcesEvenWithSavedEffectRequests)
{
    renderer::RenderSettings settings;
    auto request = HybridRequest();
    request.mode = renderer::RenderMode::RASTER;
    for (const auto pipeline : { renderer::RenderingPipeline::Forward,
                                renderer::RenderingPipeline::ForwardPlus,
                                renderer::RenderingPipeline::Deferred,
                                renderer::RenderingPipeline::DeferredPlus }) {
        settings.pipeline = pipeline;
        auto availability = PreparedResources();
        availability.raySceneReady = false;
        const auto plan = renderer::ResolveRenderPlan(settings, request, {}, {}, availability);
        EXPECT_TRUE(plan.IsValid());
        EXPECT_EQ(plan.effectiveMode, renderer::RenderMode::RASTER);
        EXPECT_EQ(plan.rasterPlan.UsesDeferredLighting(), settings.UsesGBuffer());
        EXPECT_EQ(plan.clusteredLighting, settings.UsesClusteredLighting());
        EXPECT_EQ(plan.primaryVisibility, renderer::PrimaryVisibility::RASTER);
        EXPECT_EQ(plan.rayExecution, renderer::RayExecution::NONE);
        EXPECT_FALSE(plan.NeedsRayScene());
        EXPECT_FALSE(plan.shadow.enabled);
        EXPECT_FALSE(plan.reflection.enabled);
        EXPECT_FALSE(plan.diffuseGi.enabled);
    }
}

TEST_F(ResolvedRenderPlanTest, RayPipelineSupportDoesNotSubstituteForInlineSupport)
{
    renderer::RenderSettings settings;
    const renderer::GraphicsCapabilities tierOneOnly{ true, false, true };
    const auto request = HybridRequest();
    const auto plan = renderer::ResolveRenderPlan(settings, request, tierOneOnly,
        CoveredScene(), PreparedResources());
    EXPECT_TRUE(plan.IsValid());
    EXPECT_EQ(plan.requestedMode, renderer::RenderMode::HYBRID);
    EXPECT_EQ(plan.effectiveMode, renderer::RenderMode::RASTER);
    EXPECT_EQ(plan.fallbackReason, renderer::RenderPlanReason::INLINE_RAY_QUERY_UNAVAILABLE);
    EXPECT_EQ(plan.shadow.fallbackReason, plan.fallbackReason);
    EXPECT_EQ(plan.reflection.fallbackReason, plan.fallbackReason);
    EXPECT_FALSE(plan.NeedsRayScene());
    EXPECT_TRUE(request.rayShadow);
    EXPECT_EQ(request.mode, renderer::RenderMode::HYBRID);
}

TEST_F(ResolvedRenderPlanTest, InitialRayMaterialsRequireBindlessInAdditionToInline)
{
    renderer::RenderSettings settings;
    auto capabilities = RayCapabilities();
    capabilities.bindless = false;
    const auto plan = renderer::ResolveRenderPlan(settings, HybridRequest(), capabilities,
        CoveredScene(), PreparedResources());
    EXPECT_TRUE(plan.IsValid());
    EXPECT_EQ(plan.fallbackReason, renderer::RenderPlanReason::BINDLESS_UNAVAILABLE);
    EXPECT_FALSE(plan.NeedsRayScene());
}

TEST_F(ResolvedRenderPlanTest, CoverageAndPipelineFailuresAreIsolatedByEffect)
{
    renderer::RenderSettings settings;
    auto coverage = CoveredScene();
    coverage.reflection = false;
    auto availability = PreparedResources();
    availability.diffuseGiPipelineReady = false;
    const auto plan = renderer::ResolveRenderPlan(settings, HybridRequest(), RayCapabilities(),
        coverage, availability);
    EXPECT_TRUE(plan.IsValid());
    EXPECT_EQ(plan.effectiveMode, renderer::RenderMode::HYBRID);
    EXPECT_TRUE(plan.shadow.enabled);
    EXPECT_FALSE(plan.reflection.enabled);
    EXPECT_FALSE(plan.diffuseGi.enabled);
    EXPECT_EQ(plan.reflection.fallbackReason, renderer::RenderPlanReason::RAY_COVERAGE_INCOMPLETE);
    EXPECT_EQ(plan.diffuseGi.fallbackReason, renderer::RenderPlanReason::RAY_PIPELINE_UNAVAILABLE);
    EXPECT_TRUE(plan.NeedsRayScene());
}

TEST_F(ResolvedRenderPlanTest, MissingSharedRaySceneDisablesEveryRayConsumer)
{
    renderer::RenderSettings settings;
    auto availability = PreparedResources();
    availability.raySceneReady = false;
    const auto plan = renderer::ResolveRenderPlan(settings, HybridRequest(), RayCapabilities(),
        CoveredScene(), availability);
    EXPECT_TRUE(plan.IsValid());
    EXPECT_EQ(plan.effectiveMode, renderer::RenderMode::RASTER);
    EXPECT_EQ(plan.fallbackReason, renderer::RenderPlanReason::RAY_SCENE_UNAVAILABLE);
    EXPECT_FALSE(plan.shadow.enabled);
    EXPECT_FALSE(plan.reflection.enabled);
    EXPECT_FALSE(plan.diffuseGi.enabled);
    EXPECT_FALSE(plan.NeedsRayScene());
}

TEST_F(ResolvedRenderPlanTest, HybridWithoutRequestedEffectsKeepsRasterAndNoRayDemand)
{
    renderer::RenderSettings settings;
    renderer::RenderModeRequest request;
    request.mode = renderer::RenderMode::HYBRID;
    const auto plan = renderer::ResolveRenderPlan(settings, request, RayCapabilities(),
        CoveredScene(), PreparedResources());
    EXPECT_TRUE(plan.IsValid());
    EXPECT_EQ(plan.effectiveMode, renderer::RenderMode::RASTER);
    EXPECT_EQ(plan.fallbackReason, renderer::RenderPlanReason::NO_RAY_EFFECT_REQUESTED);
    EXPECT_FALSE(plan.NeedsRayScene());
}

TEST_F(ResolvedRenderPlanTest, UnpreparedHybridPipelinesDoNotCreateAnyRayDemand)
{
    renderer::RenderSettings settings;
    auto availability = PreparedResources();
    availability.shadowPipelineReady = false;
    availability.reflectionPipelineReady = false;
    availability.diffuseGiPipelineReady = false;
    const auto plan = renderer::ResolveRenderPlan(settings, HybridRequest(), RayCapabilities(),
        CoveredScene(), availability);
    EXPECT_TRUE(plan.IsValid());
    EXPECT_EQ(plan.effectiveMode, renderer::RenderMode::RASTER);
    EXPECT_EQ(plan.fallbackReason, renderer::RenderPlanReason::RAY_PIPELINE_UNAVAILABLE);
    EXPECT_FALSE(plan.NeedsRayScene());
    EXPECT_FALSE(plan.shadow.enabled);
    EXPECT_FALSE(plan.reflection.enabled);
    EXPECT_FALSE(plan.diffuseGi.enabled);
}

TEST_F(ResolvedRenderPlanTest, MissingClusterResourcesDoNotChangeDeferredPrimaryVisibility)
{
    renderer::RenderSettings settings;
    settings.pipeline = renderer::RenderingPipeline::DeferredPlus;
    auto availability = PreparedResources();
    availability.clusteredLightingReady = false;
    const auto plan = renderer::ResolveRenderPlan(settings, {}, {}, {}, availability);
    EXPECT_TRUE(plan.IsValid());
    EXPECT_TRUE(plan.rasterPlan.UsesDeferredLighting());
    EXPECT_FALSE(plan.clusteredLighting);
    EXPECT_FALSE(plan.NeedsRayScene());
}

TEST_F(ResolvedRenderPlanTest, HybridNeedsItsRasterPrimaryPipeline)
{
    renderer::RenderSettings settings;
    auto availability = PreparedResources();
    availability.rasterPipelineReady = false;
    const auto plan = renderer::ResolveRenderPlan(settings, HybridRequest(), RayCapabilities(),
        CoveredScene(), availability);
    EXPECT_FALSE(plan.IsValid());
    EXPECT_EQ(plan.failureReason, renderer::RenderPlanReason::RASTER_PIPELINE_UNAVAILABLE);
    EXPECT_FALSE(plan.NeedsRayScene());
}

TEST_F(ResolvedRenderPlanTest, ReferenceUsesCameraRaysWithoutADeferredPrimarySurface)
{
    renderer::RenderSettings settings;
    auto availability = PreparedResources();
    availability.rasterSurfaceReady = false;
    availability.rasterPipelineReady = false;
    availability.opaque = {};
    const auto plan = renderer::ResolveRenderPlan(settings,
        PathRequest(renderer::PathTracingProfile::REFERENCE), RayCapabilities(),
        CoveredScene(), availability);
    EXPECT_TRUE(plan.IsValid());
    EXPECT_EQ(plan.effectiveMode, renderer::RenderMode::PATH_TRACING);
    EXPECT_EQ(plan.primaryVisibility, renderer::PrimaryVisibility::CAMERA_RAY);
    EXPECT_TRUE(plan.NeedsRayScene());
    EXPECT_FALSE(plan.shadow.enabled);
    EXPECT_FALSE(plan.reflection.enabled);
    EXPECT_FALSE(plan.diffuseGi.enabled);
}

TEST_F(ResolvedRenderPlanTest, GamePathRequiresDeferredAndCompleteRasterSurfaceInputs)
{
    renderer::RenderSettings settings;
    const auto request = PathRequest(renderer::PathTracingProfile::GAME);
    auto availability = PreparedResources();
    for (const auto pipeline : { renderer::RenderingPipeline::Forward,
                                renderer::RenderingPipeline::ForwardPlus,
                                renderer::RenderingPipeline::Deferred,
                                renderer::RenderingPipeline::DeferredPlus }) {
        settings.pipeline = pipeline;
        const auto plan = renderer::ResolveRenderPlan(settings, request, RayCapabilities(),
            CoveredScene(), availability);
        EXPECT_TRUE(plan.IsValid());
        EXPECT_EQ(plan.effectiveMode, settings.UsesGBuffer()
            ? renderer::RenderMode::PATH_TRACING : renderer::RenderMode::RASTER);
        EXPECT_EQ(plan.primaryVisibility, renderer::PrimaryVisibility::RASTER);
    }
    availability.rasterSurfaceReady = false;
    const auto missingSurface = renderer::ResolveRenderPlan(settings, request, RayCapabilities(),
        CoveredScene(), availability);
    EXPECT_EQ(missingSurface.effectiveMode, renderer::RenderMode::RASTER);
    EXPECT_EQ(missingSurface.fallbackReason, renderer::RenderPlanReason::RASTER_SURFACE_UNAVAILABLE);
    EXPECT_FALSE(missingSurface.NeedsRayScene());
}

TEST_F(ResolvedRenderPlanTest, ForwardPrepassAfterDeferredFailureCannotStartGamePath)
{
    renderer::RenderSettings settings;
    settings.pipeline = renderer::RenderingPipeline::Deferred;
    settings.ssr.enabled = true;
    auto availability = PreparedResources();
    availability.opaque.deferredLighting = false;
    const auto plan = renderer::ResolveRenderPlan(settings,
        PathRequest(renderer::PathTracingProfile::GAME), RayCapabilities(), CoveredScene(), availability);
    EXPECT_TRUE(plan.IsValid());
    EXPECT_EQ(plan.rasterPlan.path, renderer::OpaqueRenderPath::FORWARD_DEPTH_NORMAL);
    EXPECT_EQ(plan.effectiveMode, renderer::RenderMode::RASTER);
    EXPECT_EQ(plan.fallbackReason, renderer::RenderPlanReason::RASTER_SURFACE_UNAVAILABLE);
    EXPECT_FALSE(plan.NeedsRayScene());
}

TEST_F(ResolvedRenderPlanTest, PathCoverageFailureDoesNotLeaveSomeRayEffectsActive)
{
    renderer::RenderSettings settings;
    auto request = PathRequest(renderer::PathTracingProfile::REFERENCE);
    request.rayShadow = true;
    request.rayReflection = true;
    auto coverage = CoveredScene();
    coverage.path = false;
    const auto plan = renderer::ResolveRenderPlan(settings, request, RayCapabilities(),
        coverage, PreparedResources());
    EXPECT_TRUE(plan.IsValid());
    EXPECT_EQ(plan.effectiveMode, renderer::RenderMode::RASTER);
    EXPECT_EQ(plan.fallbackReason, renderer::RenderPlanReason::RAY_COVERAGE_INCOMPLETE);
    EXPECT_FALSE(plan.shadow.enabled);
    EXPECT_FALSE(plan.reflection.enabled);
    EXPECT_FALSE(plan.NeedsRayScene());
}

TEST_F(ResolvedRenderPlanTest, HardwareAndCoverageAloneDoNotEnableAnUnpreparedPathPipeline)
{
    renderer::RenderSettings settings;
    auto availability = PreparedResources();
    availability.pathPipelineReady = false;
    const auto plan = renderer::ResolveRenderPlan(settings,
        PathRequest(renderer::PathTracingProfile::REFERENCE), RayCapabilities(), CoveredScene(), availability);
    EXPECT_TRUE(plan.IsValid());
    EXPECT_EQ(plan.effectiveMode, renderer::RenderMode::RASTER);
    EXPECT_EQ(plan.fallbackReason, renderer::RenderPlanReason::RAY_PIPELINE_UNAVAILABLE);
    EXPECT_FALSE(plan.NeedsRayScene());
}

TEST_F(ResolvedRenderPlanTest, MissingFallbackResourcesKeepTheOriginalReasonAndFailTheFrame)
{
    renderer::RenderSettings settings;
    auto availability = PreparedResources();
    availability.pathPipelineReady = false;
    availability.rasterPipelineReady = false;
    const auto plan = renderer::ResolveRenderPlan(settings,
        PathRequest(renderer::PathTracingProfile::REFERENCE), RayCapabilities(), CoveredScene(), availability);
    EXPECT_FALSE(plan.IsValid());
    EXPECT_EQ(plan.fallbackReason, renderer::RenderPlanReason::RAY_PIPELINE_UNAVAILABLE);
    EXPECT_EQ(plan.failureReason, renderer::RenderPlanReason::RASTER_PIPELINE_UNAVAILABLE);
    EXPECT_FALSE(plan.NeedsRayScene());
}

TEST_F(ResolvedRenderPlanTest, InvalidModesAndProfilesResolveToPreparedRaster)
{
    renderer::RenderSettings settings;
    auto request = HybridRequest();
    request.mode = static_cast<renderer::RenderMode>(255);
    const auto invalidMode = renderer::ResolveRenderPlan(settings, request, RayCapabilities(),
        CoveredScene(), PreparedResources());
    EXPECT_TRUE(invalidMode.IsValid());
    EXPECT_EQ(invalidMode.fallbackReason, renderer::RenderPlanReason::INVALID_REQUEST);
    EXPECT_FALSE(invalidMode.NeedsRayScene());
    request = PathRequest(static_cast<renderer::PathTracingProfile>(255));
    const auto invalidProfile = renderer::ResolveRenderPlan(settings, request, RayCapabilities(),
        CoveredScene(), PreparedResources());
    EXPECT_TRUE(invalidProfile.IsValid());
    EXPECT_EQ(invalidProfile.fallbackReason, renderer::RenderPlanReason::INVALID_REQUEST);
    EXPECT_FALSE(invalidProfile.NeedsRayScene());
}

}
}
