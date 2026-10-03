/// @file    RenderSettingsTests.cpp
/// @brief   Game View 用に診断表示を外した設定が、診断表示だけを落とすことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// @note Scene View の診断表示を Game View へ漏らさず、画質設定は保持する。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Renderer/RenderSettings.hpp>
#include <initializer_list>
#include <limits>

namespace fbzz::tests {

using renderer::RenderSettings;
using renderer::RenderingPipeline;
using renderer::ViewMode;

class RenderSettingsTest : public testkit::EngineFixture {};

namespace {

RenderSettings MakeEveryDiagnosticOn()
{
    RenderSettings settings;
    settings.viewMode                      = ViewMode::WireframeUnlit;
    settings.showColliders                 = true;
    settings.showTerrainCollision          = true;
    settings.showDecalBounds               = true;
    settings.showNavMesh                   = true;
    settings.showNavSensors                = true;
    settings.showSkeleton                  = true;
    settings.showGrid                      = true;
    settings.showLightRange                = true;
    settings.showVFXGizmos                 = true;
    settings.showFlowFields                = true;
    settings.showFlowSamples               = true;
    settings.showPhysicsVolumes            = true;
    settings.showWaterFlow                 = true;
    settings.showConstraints               = true;
    settings.showRagdoll                   = true;
    settings.showScriptGizmos              = true;
    settings.showRigidBodies               = true;
    settings.showIK                        = true;
    settings.showSpringBones               = true;
    settings.showAttachments               = true;
    settings.showVFXPaths                  = true;
    settings.showTerrainBounds             = true;
    settings.showLODBounds                 = true;
    settings.showUIRects                   = true;
    settings.passViewerEnabled             = true;
    settings.particleOverdrawView          = true;
    settings.particleOverdrawReadback      = true;
    settings.shadow.debugVisualizeCascades = true;
    settings.clustered.debugHeatmap        = true;
    return settings;
}

} /// @note namespace

TEST_F(RenderSettingsTest, StripDebugVisualizationTurnsOffEveryDiagnosticView)
{
    RenderSettings settings = MakeEveryDiagnosticOn();
    settings.StripDebugVisualization();

    EXPECT_TRUE(settings.viewMode == ViewMode::Lit);
    EXPECT_FALSE(settings.showColliders);
    EXPECT_FALSE(settings.showTerrainCollision);
    EXPECT_FALSE(settings.showDecalBounds);
    EXPECT_FALSE(settings.showNavMesh);
    EXPECT_FALSE(settings.showNavSensors);
    EXPECT_FALSE(settings.showSkeleton);
    EXPECT_FALSE(settings.showGrid);
    EXPECT_FALSE(settings.showLightRange);
    EXPECT_FALSE(settings.showVFXGizmos);
    EXPECT_FALSE(settings.showFlowFields);
    EXPECT_FALSE(settings.showFlowSamples);
    EXPECT_FALSE(settings.showPhysicsVolumes);
    EXPECT_FALSE(settings.showWaterFlow);
    EXPECT_FALSE(settings.showConstraints);
    EXPECT_FALSE(settings.showRagdoll);
    EXPECT_FALSE(settings.showScriptGizmos);
    EXPECT_FALSE(settings.showRigidBodies);
    EXPECT_FALSE(settings.showIK);
    EXPECT_FALSE(settings.showSpringBones);
    EXPECT_FALSE(settings.showAttachments);
    EXPECT_FALSE(settings.showVFXPaths);
    EXPECT_FALSE(settings.showTerrainBounds);
    EXPECT_FALSE(settings.showLODBounds);
    EXPECT_FALSE(settings.passViewerEnabled);
    EXPECT_FALSE(settings.particleOverdrawView);
    EXPECT_FALSE(settings.particleOverdrawReadback);
    EXPECT_FALSE(settings.shadow.debugVisualizeCascades);
    EXPECT_FALSE(settings.clustered.debugHeatmap);
}

TEST_F(RenderSettingsTest, StripDebugVisualizationKeepsUIRects)
{
    /// @note UI の当たり判定を試すクリックは Game View で行う。ここで消すと矩形を見ながら押せない。
    RenderSettings settings = MakeEveryDiagnosticOn();
    settings.StripDebugVisualization();

    EXPECT_TRUE(settings.showUIRects);
}

TEST_F(RenderSettingsTest, ScriptGizmosAreOffByDefault)
{
    /// @note 配布ビルドは Strip を通さず既定値で描くので、既定が true だとゲーム画面にギズモが乗る。
    const RenderSettings settings;
    EXPECT_FALSE(settings.showScriptGizmos);
}

TEST_F(RenderSettingsTest, StripDebugVisualizationKeepsSkeletonFilterMode)
{
    /// @note «選択中だけ» は表示ではなくモード。Scene View へ戻ったときに選び直させない。
    RenderSettings settings = MakeEveryDiagnosticOn();
    settings.skeletonSelectedOnly = false;
    settings.StripDebugVisualization();

    EXPECT_FALSE(settings.skeletonSelectedOnly);
}

TEST_F(RenderSettingsTest, IsSelectedMatchesIndexAndGeneration)
{
    RenderSettings settings;
    settings.selectedObjects.push_back({ 7u, 3u });

    EXPECT_TRUE(settings.IsSelected(7u, 3u));
    EXPECT_FALSE(settings.IsSelected(7u, 4u));
    EXPECT_FALSE(settings.IsSelected(8u, 3u));
}

TEST_F(RenderSettingsTest, StripDebugVisualizationKeepsLookSettings)
{
    RenderSettings settings;
    settings.pipeline       = RenderingPipeline::DeferredPlus;
    settings.shadowEnabled  = false;
    settings.renderScale    = 0.7f;
    settings.userBrightness = 1.5f;
    settings.particleBudget = 1234;
    settings.clustered.maxDistance = 90.0f;
    settings.StripDebugVisualization();

    EXPECT_TRUE(settings.pipeline == RenderingPipeline::DeferredPlus);
    EXPECT_FALSE(settings.shadowEnabled);
    EXPECT_FLOAT_EQ(settings.renderScale, 0.7f);
    EXPECT_FLOAT_EQ(settings.userBrightness, 1.5f);
    EXPECT_EQ(settings.particleBudget, 1234);
    EXPECT_FLOAT_EQ(settings.clustered.maxDistance, 90.0f);
}

TEST_F(RenderSettingsTest, HybridWorkloadPresetsAreValidAndLeaveOtherSettingsAlone)
{
    RenderSettings settings;
    settings.modeRequest.mode = renderer::RenderMode::RASTER;
    settings.renderScale = 0.75f;
    settings.ssr.enabled = false;
    for (auto preset : {renderer::HybridQualityPreset::LOW, renderer::HybridQualityPreset::BALANCED,
        renderer::HybridQualityPreset::HIGH, renderer::HybridQualityPreset::PERFORMANCE}) {
        settings.hybridQuality = renderer::MakeHybridQualityPreset(preset);
        EXPECT_TRUE(renderer::IsHybridQualityValid(settings.hybridQuality));
        EXPECT_EQ(renderer::DetectHybridQualityPreset(settings.hybridQuality), preset);
        settings.StripDebugVisualization();
        EXPECT_EQ(renderer::DetectHybridQualityPreset(settings.hybridQuality), preset);
        EXPECT_EQ(settings.modeRequest.mode, renderer::RenderMode::RASTER);
        EXPECT_FALSE(settings.ssr.enabled);
        EXPECT_FLOAT_EQ(settings.renderScale, 0.75f);
    }
    settings.hybridQuality.reflectionSamples = 3;
    EXPECT_EQ(renderer::DetectHybridQualityPreset(settings.hybridQuality), renderer::HybridQualityPreset::CUSTOM);
}

TEST_F(RenderSettingsTest, HybridWorkloadRejectsUnsafeCountsAndNonFiniteTargets)
{
    renderer::HybridQualitySettings quality;
    EXPECT_TRUE(renderer::IsHybridQualityValid(quality));
    quality.reflectionSamples = 0;
    EXPECT_FALSE(renderer::IsHybridQualityValid(quality));
    quality.reflectionSamples = 65;
    EXPECT_FALSE(renderer::IsHybridQualityValid(quality));
    quality = {};
    quality.maxTraceDistance = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(renderer::IsHybridQualityValid(quality));
    quality = {};
    quality.frameBudgetMs = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(renderer::IsHybridQualityValid(quality));
    quality = {};
    quality.maxHistoryMiB = 0;
    EXPECT_FALSE(renderer::IsHybridQualityValid(quality));
}

TEST_F(RenderSettingsTest, PerformanceQualityLimitsTransportAndKeepsLegacyDefaults)
{
    const renderer::HybridQualitySettings legacy;
    EXPECT_EQ(legacy.reflectionResolutionDivisor, 1u);
    EXPECT_FALSE(legacy.glassStochastic);
    const auto quality = renderer::MakeHybridQualityPreset(renderer::HybridQualityPreset::PERFORMANCE);
    EXPECT_EQ(quality.reflectionSamples, 1u);
    EXPECT_EQ(quality.reflectionResolutionDivisor, 2u);
    EXPECT_TRUE(quality.glassStochastic);
    EXPECT_EQ(quality.glassBoundaryLimit, legacy.glassBoundaryLimit);
    EXPECT_FLOAT_EQ(quality.frameBudgetMs, 1000.0f / 120.0f);
    EXPECT_TRUE(renderer::IsHybridQualityValid(quality));
    for (uint32_t divisor : {0u, 3u, 4u}) {
        auto invalid = quality;
        invalid.reflectionResolutionDivisor = divisor;
        EXPECT_FALSE(renderer::IsHybridQualityValid(invalid));
    }
}

} /// @note namespace fbzz::tests
