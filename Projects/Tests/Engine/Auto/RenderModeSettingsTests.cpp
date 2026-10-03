/// @file    RenderModeSettingsTests.cpp
/// @brief   描画モード要求の保存互換・不正入力・縮退後の要求保持を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-30
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/ProjectSettings.hpp>
#include <Engine/Input/InputActionMap.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Graphics/Pipeline/ResolvedRenderPlan.hpp>
#include <fstream>

namespace fbzz::tests {
namespace {

class RenderModeSettingsTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        testkit::EngineFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
        input::InputActionMap::Clear();
    }
    void TearDown() override
    {
        input::InputActionMap::Clear();
        testkit::EngineFixture::TearDown();
    }
    std::string File() const
    {
        return util::FileSystem::PathToUtf8(m_temp.File("ProjectSettings.toml"));
    }
    void Write(const char* text) const
    {
        std::ofstream out(m_temp.File("ProjectSettings.toml"), std::ios::binary);
        out << text;
    }
    void ExpectRasterDefault(const renderer::RenderModeRequest& request) const
    {
        EXPECT_EQ(request.mode, renderer::RenderMode::RASTER);
        EXPECT_EQ(request.pathProfile, renderer::PathTracingProfile::REFERENCE);
        EXPECT_FALSE(request.rayShadow);
        EXPECT_FALSE(request.rayReflection);
        EXPECT_FALSE(request.rayDiffuseGi);
    }
    testkit::TempDir m_temp{"render-mode"};
};

TEST_F(RenderModeSettingsTest, OldSettingsResetAnEarlierRayRequest)
{
    for (const auto text : { "[render]\npipeline = 'Deferred+'\n", "[physics]\nhz = 60\n" }) {
        Write(text);
        ProjectSettings settings;
        settings.render.modeRequest = { renderer::RenderMode::PATH_TRACING,
            renderer::PathTracingProfile::GAME, true, true, true };
        ASSERT_TRUE(settings.Load(File()));
        ExpectRasterDefault(settings.render.modeRequest);
    }
}

TEST_F(RenderModeSettingsTest, InvalidKeysDoNotEnableRayTracing)
{
    Write("[render]\nmode = 'FutureRenderer'\npathProfile = 3\n"
          "rayShadow = 'true'\nrayReflection = 1\nrayDiffuseGi = []\n");
    ProjectSettings settings;
    ASSERT_TRUE(settings.Load(File()));
    ExpectRasterDefault(settings.render.modeRequest);
}

TEST_F(RenderModeSettingsTest, EveryModeAndProfileRoundTripsDormantEffectRequests)
{
    for (const auto mode : { renderer::RenderMode::RASTER, renderer::RenderMode::HYBRID,
                            renderer::RenderMode::PATH_TRACING }) {
        for (const auto profile : { renderer::PathTracingProfile::REFERENCE,
                                   renderer::PathTracingProfile::GAME }) {
            ProjectSettings source;
            source.render.pipeline = renderer::RenderingPipeline::DeferredPlus;
            source.render.modeRequest = { mode, profile, true, false, true };
            ASSERT_TRUE(source.Save(File()));
            ProjectSettings restored;
            ASSERT_TRUE(restored.Load(File()));
            EXPECT_EQ(restored.render.pipeline, source.render.pipeline);
            EXPECT_EQ(restored.render.modeRequest.mode, mode);
            EXPECT_EQ(restored.render.modeRequest.pathProfile, profile);
            EXPECT_TRUE(restored.render.modeRequest.rayShadow);
            EXPECT_FALSE(restored.render.modeRequest.rayReflection);
            EXPECT_TRUE(restored.render.modeRequest.rayDiffuseGi);
        }
    }
}

TEST_F(RenderModeSettingsTest, RasterFallbackDoesNotChangeTheSavedRequest)
{
    ProjectSettings source;
    source.render.modeRequest = { renderer::RenderMode::HYBRID,
        renderer::PathTracingProfile::GAME, false, true, false };
    renderer::RenderAvailability available;
    available.outputsReady = true;
    available.rasterPipelineReady = true;
    const auto plan = renderer::ResolveRenderPlan(source.render, source.render.modeRequest,
        {}, {}, available);
    ASSERT_TRUE(plan.IsValid());
    EXPECT_EQ(plan.effectiveMode, renderer::RenderMode::RASTER);
    ASSERT_TRUE(source.Save(File()));
    ProjectSettings restored;
    ASSERT_TRUE(restored.Load(File()));
    EXPECT_EQ(restored.render.modeRequest.mode, renderer::RenderMode::HYBRID);
    EXPECT_EQ(restored.render.modeRequest.pathProfile, renderer::PathTracingProfile::GAME);
    EXPECT_TRUE(restored.render.modeRequest.rayReflection);
}

TEST_F(RenderModeSettingsTest, DeveloperGateKeepsEverySavedRayModeAndDormantEffect)
{
    for (const auto mode : { renderer::RenderMode::HYBRID, renderer::RenderMode::PATH_TRACING }) {
        for (const auto profile : { renderer::PathTracingProfile::REFERENCE,
                                   renderer::PathTracingProfile::GAME }) {
            ProjectSettings source;
            source.render.pipeline = renderer::RenderingPipeline::DeferredPlus;
            source.render.modeRequest = { mode, profile, true, false, true };
            renderer::RenderAvailability available;
            available.outputsReady = true;
            available.rasterPipelineReady = true;
            available.opaque = { true, true, true };
            available.clusteredLightingReady = true;
            available.raySceneReady = true;
            available.shadowPipelineReady = true;
            available.reflectionPipelineReady = true;
            available.diffuseGiPipelineReady = true;
            available.pathPipelineReady = true;
            available.rasterSurfaceReady = true;
            const auto plan = renderer::ResolveRenderPlan(source.render, source.render.modeRequest,
                { true, true, true }, { true, true, true, true }, available);
            ASSERT_TRUE(plan.IsValid());
            EXPECT_EQ(plan.requestedMode, mode);
            EXPECT_EQ(plan.pathProfile, profile);
            EXPECT_EQ(plan.effectiveMode, renderer::RenderMode::RASTER);
            EXPECT_EQ(plan.fallbackReason, renderer::RenderPlanReason::DEVELOPER_MODE_REQUIRED);
            EXPECT_FALSE(plan.NeedsRayScene());
            ASSERT_TRUE(source.Save(File()));
            ProjectSettings restored;
            ASSERT_TRUE(restored.Load(File()));
            EXPECT_EQ(restored.render.pipeline, source.render.pipeline);
            EXPECT_EQ(restored.render.modeRequest.mode, mode);
            EXPECT_EQ(restored.render.modeRequest.pathProfile, profile);
            EXPECT_TRUE(restored.render.modeRequest.rayShadow);
            EXPECT_FALSE(restored.render.modeRequest.rayReflection);
            EXPECT_TRUE(restored.render.modeRequest.rayDiffuseGi);
        }
    }
}

TEST_F(RenderModeSettingsTest, ParseFailurePreservesTheEarlierRequest)
{
    Write("[render\nmode = 'Hybrid'\n");
    ProjectSettings settings;
    settings.render.modeRequest = { renderer::RenderMode::HYBRID,
        renderer::PathTracingProfile::GAME, true, true, true };
    EXPECT_FALSE(settings.Load(File()));
    EXPECT_EQ(settings.render.modeRequest.mode, renderer::RenderMode::HYBRID);
    EXPECT_EQ(settings.render.modeRequest.pathProfile, renderer::PathTracingProfile::GAME);
    EXPECT_TRUE(settings.render.modeRequest.rayShadow);
    EXPECT_TRUE(settings.render.modeRequest.rayReflection);
    EXPECT_TRUE(settings.render.modeRequest.rayDiffuseGi);
}

}
}
