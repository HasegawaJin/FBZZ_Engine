/// @file    GeometryPipelineTests.cpp
/// @brief   描画資源の欠落時の経路と、方式ごとの登録順を固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#include <TestKit/TestKit.hpp>
#include <Engine/Renderer/OpaqueRenderPlan.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <Engine/Scene/Systems/RenderPasses/GeometryPipeline.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPipeline.hpp>
#include <algorithm>
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {

using renderer::OpaqueRenderAvailability;
using renderer::OpaqueRenderPath;
using renderer::OpaqueRenderPlan;
using renderer::RenderingPipeline;
using renderer::ResolveOpaqueRenderPlan;

renderer::RenderSettings SettingsWithoutScreenSpaceEffects()
{
    renderer::RenderSettings settings;
    settings.postProcess.ambientOcclusion.enabled = false;
    settings.gtao.enabled = false;
    settings.contactShadow.enabled = false;
    settings.ssr.enabled = false;
    return settings;
}

class GeometryPipelineTest : public testkit::Fixture {};

TEST_F(GeometryPipelineTest, LightingListChoiceDoesNotChangeTheOpaqueTechnique)
{
    const OpaqueRenderAvailability available{ true, true, true };
    auto settings = SettingsWithoutScreenSpaceEffects();
    struct PipelineCase {
        RenderingPipeline pipeline;
        OpaqueRenderPath expected;
    };
    const PipelineCase cases[] = {
        { RenderingPipeline::Forward, OpaqueRenderPath::FORWARD },
        { RenderingPipeline::ForwardPlus, OpaqueRenderPath::FORWARD },
        { RenderingPipeline::Deferred, OpaqueRenderPath::DEFERRED },
        { RenderingPipeline::DeferredPlus, OpaqueRenderPath::DEFERRED },
    };
    for (const auto& testCase : cases) {
        settings.pipeline = testCase.pipeline;
        for (const bool clustered : { false, true }) {
            settings.clustered.enabled = clustered;
            for (const bool forceAllLights : { false, true }) {
                settings.clustered.forceAllLights = forceAllLights;
                EXPECT_EQ(ResolveOpaqueRenderPlan(settings, available).path, testCase.expected);
            }
        }
    }
}

TEST_F(GeometryPipelineTest, EachDeferredResourceIsRequiredBeforeSelectingDeferred)
{
    auto settings = SettingsWithoutScreenSpaceEffects();
    settings.pipeline = RenderingPipeline::Deferred;
    for (const OpaqueRenderAvailability missing : {
             OpaqueRenderAvailability{ false, true, true },
             OpaqueRenderAvailability{ true, false, true },
             OpaqueRenderAvailability{ true, true, false } }) {
        const auto plan = ResolveOpaqueRenderPlan(settings, missing);
        EXPECT_EQ(plan.path, OpaqueRenderPath::FORWARD);
        EXPECT_FALSE(plan.UsesDeferredLighting());
        EXPECT_FALSE(plan.HasScreenSpaceInputs());
    }
}

TEST_F(GeometryPipelineTest, DeferredFallbackCanStillProvideForwardScreenSpaceInputs)
{
    auto settings = SettingsWithoutScreenSpaceEffects();
    settings.pipeline = RenderingPipeline::DeferredPlus;
    settings.ssr.enabled = true;
    for (const OpaqueRenderAvailability missing : {
             OpaqueRenderAvailability{ true, false, true },
             OpaqueRenderAvailability{ true, true, false } }) {
        const auto plan = ResolveOpaqueRenderPlan(settings, missing);
        EXPECT_EQ(plan.path, OpaqueRenderPath::FORWARD_DEPTH_NORMAL);
        EXPECT_FALSE(plan.UsesDeferredLighting());
        EXPECT_TRUE(plan.HasScreenSpaceInputs());
    }
    EXPECT_EQ(ResolveOpaqueRenderPlan(settings, { false, true, true }).path,
              OpaqueRenderPath::FORWARD);
}

TEST_F(GeometryPipelineTest, EveryScreenSpaceConsumerRequestsTheForwardPrepass)
{
    for (int effect = 0; effect < 4; ++effect) {
        auto settings = SettingsWithoutScreenSpaceEffects();
        settings.pipeline = RenderingPipeline::Forward;
        switch (effect) {
        case 0: settings.postProcess.ambientOcclusion.enabled = true; break;
        case 1: settings.gtao.enabled = true; break;
        case 2: settings.contactShadow.enabled = true; break;
        case 3: settings.ssr.enabled = true; break;
        }
        EXPECT_EQ(ResolveOpaqueRenderPlan(settings, { true, false, false }).path,
                  OpaqueRenderPath::FORWARD_DEPTH_NORMAL);
        EXPECT_EQ(ResolveOpaqueRenderPlan(settings, {}).path, OpaqueRenderPath::FORWARD);
    }
}

TEST_F(GeometryPipelineTest, UnlitSkipsOnlyTheForwardScreenSpacePrepass)
{
    auto settings = SettingsWithoutScreenSpaceEffects();
    settings.ssr.enabled = true;
    settings.viewMode = renderer::ViewMode::Unlit;
    settings.pipeline = RenderingPipeline::Forward;
    EXPECT_EQ(ResolveOpaqueRenderPlan(settings, { true, true, true }).path,
              OpaqueRenderPath::FORWARD);
    settings.pipeline = RenderingPipeline::Deferred;
    EXPECT_EQ(ResolveOpaqueRenderPlan(settings, { true, true, true }).path,
              OpaqueRenderPath::DEFERRED);
}

TEST_F(GeometryPipelineTest, WireframeUsesForwardColorWithoutAnOpaquePrepass)
{
    auto settings = SettingsWithoutScreenSpaceEffects();
    settings.ssr.enabled = true;
    for (const auto mode : { renderer::ViewMode::WireframeLit, renderer::ViewMode::WireframeUnlit }) {
        settings.viewMode = mode;
        for (const auto pipeline : { RenderingPipeline::Forward, RenderingPipeline::ForwardPlus,
                                     RenderingPipeline::Deferred, RenderingPipeline::DeferredPlus }) {
            settings.pipeline = pipeline;
            EXPECT_EQ(ResolveOpaqueRenderPlan(settings, { true, true, true }).path,
                      OpaqueRenderPath::FORWARD);
        }
    }
}

TEST_F(GeometryPipelineTest, RegistrationPreservesTheExistingPassOrderForEveryPath)
{
    /// @note 媒体 atlas は照明前、界面は体積光前、SSR の解決は透明描画前に登録する。
    /// @see Docs/design/water-gameplay-visuals.md
    /// @see Docs/design/graphics-quality.md
    const std::vector<std::string> forward{
        "DepthPrePass", "BuildOcclusionHiZ", "CullOccludedMeshes",
        "ForwardOpaque", "TerrainForward", "Sky", "SunMoon", "VolumetricCloud",
    };
    const std::vector<std::string> forwardPrepass{
        "DepthPrePass", "BuildOcclusionHiZ", "CullOccludedMeshes",
        "DepthNormalPrepass", "TerrainGBuffer", "GTAO", "ContactShadows", "SSAO",
        "ForwardOpaque", "TerrainForward", "Sky", "SunMoon", "VolumetricCloud", "SSR", "SSRResolve",
    };
    const std::vector<std::string> deferred{
        "DepthPrePass", "BuildOcclusionHiZ", "CullOccludedMeshes",
        "DeferredGBuffer", "TerrainGBuffer", "DeferredDepthCopy", "GTAO", "ContactShadows",
        "SSAO", "DeferredLighting", "Sky", "SunMoon", "VolumetricCloud",
        "DeferredSkinnedForward", "FiberForward", "SSR", "SSRResolve",
    };

    for (const auto path : { OpaqueRenderPath::FORWARD, OpaqueRenderPath::FORWARD_DEPTH_NORMAL,
                             OpaqueRenderPath::DEFERRED }) {
        for (const bool clustered : { false, true }) {
            scene::RenderPipeline pipeline;
            pipeline.BeginBuild();
            scene::BuildGeometryPreparation(pipeline, clustered);
            scene::BuildGeometryPipeline(pipeline, OpaqueRenderPlan{ path });
            pipeline.AddRawPass("AfterOpaque", {}, {}, []() {});
            scene::BuildGeometryTransparency(pipeline, OpaqueRenderPlan{ path });
            scene::BuildWaterComposition(pipeline);

            std::vector<std::string> expected{ "SkinningCompute" };
            if (clustered)
                expected.emplace_back("ClusterLightCull");
            expected.emplace_back("LightCookie");
            expected.emplace_back("Shadow");
            expected.emplace_back("WaterMediumAtlas");
            const auto& geometry = path == OpaqueRenderPath::DEFERRED ? deferred
                : path == OpaqueRenderPath::FORWARD_DEPTH_NORMAL ? forwardPrepass : forward;
            expected.insert(expected.end(), geometry.begin(), geometry.end());
            expected.emplace_back("AfterOpaque");
            expected.emplace_back(path == OpaqueRenderPath::DEFERRED ? "DeferredForwardTransparent" : "ForwardTransparent");
            expected.insert(expected.end(), { "WaterInterface", "VolumetricLight", "WaterCaustics", "WaterForward" });
            EXPECT_EQ(pipeline.RegisteredPassNames(), expected);
        }
    }
}

TEST_F(GeometryPipelineTest, HybridScreenFirstFreezesBaselineBeforeTracingAndResolvesBeforeTransparency)
{
    scene::RenderPipeline pipeline;
    scene::BuildGeometryPipeline(pipeline, { OpaqueRenderPath::DEFERRED }, [&]() {
        pipeline.AddRawPass("RayReflection", {}, {}, []() {});
    }, true);
    const std::vector<std::string> expected{
        "DepthPrePass", "BuildOcclusionHiZ", "CullOccludedMeshes",
        "DeferredGBuffer", "TerrainGBuffer", "DeferredDepthCopy", "GTAO", "ContactShadows", "SSAO",
        "ReflectionSourceLighting", "Sky", "SunMoon", "SSR", "RayReflection",
        "DeferredLighting", "VolumetricCloud", "DeferredSkinnedForward", "FiberForward",
    };
    EXPECT_EQ(pipeline.RegisteredPassNames(), expected);
    const auto names = pipeline.RegisteredPassNames();
    EXPECT_EQ(std::count(names.begin(), names.end(), "SSR"), 1);
    EXPECT_EQ(std::count(names.begin(), names.end(), "Sky"), 1);
    EXPECT_EQ(std::count(names.begin(), names.end(), "VolumetricCloud"), 1);
    EXPECT_LT(std::find(names.begin(), names.end(), "DeferredLighting"),
        std::find(names.begin(), names.end(), "VolumetricCloud"));
}

TEST_F(GeometryPipelineTest, SwitchingTechniqueRebuildsWithoutLeavingThePreviousPath)
{
    scene::RenderPipeline pipeline;
    pipeline.BeginBuild();
    scene::BuildGeometryPipeline(pipeline, { OpaqueRenderPath::DEFERRED });
    pipeline.BeginBuild();
    scene::BuildGeometryPipeline(pipeline, { OpaqueRenderPath::FORWARD });
    const auto names = pipeline.RegisteredPassNames();
    EXPECT_EQ(std::count(names.begin(), names.end(), "ForwardOpaque"), 1);
    EXPECT_EQ(std::count(names.begin(), names.end(), "DeferredLighting"), 0);
    EXPECT_EQ(std::count(names.begin(), names.end(), "DeferredGBuffer"), 0);
}

} /// @note namespace
} /// @note namespace fbzz::tests
