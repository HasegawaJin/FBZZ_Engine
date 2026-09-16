/// @file    RenderSettingsTests.cpp
/// @brief   Game View 用に診断表示を外した設定が、診断表示だけを落とすことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// Scene View で点けた診断表示が Game View へ漏れると «ゲームの絵が壊れた» ように見える。
/// 逆に絵作りの設定まで落とすと Game View だけ見た目が変わる。両方向を固定する。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Renderer/RenderSettings.hpp>

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
    settings.showConstraints               = true;
    settings.showRagdoll                   = true;
    settings.showUIRects                   = true;
    settings.passViewerEnabled             = true;
    settings.particleOverdrawView          = true;
    settings.particleOverdrawReadback      = true;
    settings.shadow.debugVisualizeCascades = true;
    settings.clustered.debugHeatmap        = true;
    return settings;
}

} // namespace

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
    EXPECT_FALSE(settings.showConstraints);
    EXPECT_FALSE(settings.showRagdoll);
    EXPECT_FALSE(settings.passViewerEnabled);
    EXPECT_FALSE(settings.particleOverdrawView);
    EXPECT_FALSE(settings.particleOverdrawReadback);
    EXPECT_FALSE(settings.shadow.debugVisualizeCascades);
    EXPECT_FALSE(settings.clustered.debugHeatmap);
}

TEST_F(RenderSettingsTest, StripDebugVisualizationKeepsUIRects)
{
    // UI の当たり判定を試すクリックは Game View で行う。ここで消すと矩形を見ながら押せない。
    RenderSettings settings = MakeEveryDiagnosticOn();
    settings.StripDebugVisualization();

    EXPECT_TRUE(settings.showUIRects);
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

} // namespace fbzz::tests
