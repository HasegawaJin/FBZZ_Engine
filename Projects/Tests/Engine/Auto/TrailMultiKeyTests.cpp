/// @file    TrailMultiKeyTests.cpp
/// @brief   Trail の瞬間移動切断と、多キー幅 / 色の «既定では従来どおり» を固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// この 2 つはどちらも «既存シーンの見た目を 1 ドットも変えない» ことが要件だった。
/// 既定値を変えると、出荷済みの .scene に入っている剣閃や乗り物の軌跡が
/// «たまに途切れる» / «太さが変わる» という形でだけ壊れる ── 絵を見比べないと
/// 気づけないので、既定の答えを数値で固定しておく。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Renderer/RenderSettings.hpp>
#include <Engine/Scene/Components/TrailComponent.hpp>

namespace fbzz::tests {
namespace {

/// 0 → 1 へ立ち上がるだけの 2 キーのカーブ。
scene::ParticleCurve Ramp()
{
    scene::ParticleCurve curve;
    curve.keys[0]       = { 0.0f, 0.0f };
    curve.keys[1]       = { 1.0f, 1.0f };
    curve.keyCount      = 2;
    curve.interpolation = scene::ParticleCurveInterpolation::Linear;
    return curve;
}

} // namespace

class TrailMultiKeyTest : public testkit::EngineFixture {};

/// @name 瞬間移動の切断

TEST_F(TrailMultiKeyTest, NeverBreaksWithTheDefaultThreshold)
{
    const scene::TrailComponent trail;

    ASSERT_EQ(trail.breakDistance, 0.0f);
    EXPECT_FALSE(scene::TrailIsDiscontinuous(trail, { 0.0f, 0.0f, 0.0f }, { 900.0f, 0.0f, 0.0f }));
}

TEST_F(TrailMultiKeyTest, BreaksOnlyBeyondTheThreshold)
{
    scene::TrailComponent trail;
    trail.breakDistance = 2.0f;

    EXPECT_FALSE(scene::TrailIsDiscontinuous(trail, { 0.0f, 0.0f, 0.0f }, { 1.9f, 0.0f, 0.0f }));
    EXPECT_FALSE(scene::TrailIsDiscontinuous(trail, { 0.0f, 0.0f, 0.0f }, { 2.0f, 0.0f, 0.0f }));
    EXPECT_TRUE(scene::TrailIsDiscontinuous(trail, { 0.0f, 0.0f, 0.0f }, { 2.1f, 0.0f, 0.0f }));
}

TEST_F(TrailMultiKeyTest, MeasuresTheJumpInThreeDimensions)
{
    scene::TrailComponent trail;
    trail.breakDistance = 1.0f;

    /// @note 各軸 0.6 は 1 軸だけ見れば閾値の内側だが、長さは 1.04 で外側。
    EXPECT_TRUE(scene::TrailIsDiscontinuous(trail, { 0.0f, 0.0f, 0.0f }, { 0.6f, 0.6f, 0.6f }));
}

/// @name 多キーの幅

TEST_F(TrailMultiKeyTest, FallsBackToTheTwoEndpointsByDefault)
{
    scene::TrailComponent trail;
    trail.widthStart = 0.20f;
    trail.widthEnd   = 0.02f;

    ASSERT_FALSE(trail.widthCurveEnabled);
    ASSERT_FALSE(scene::TrailUsesWidthCurve(trail));
    /// @note age=1 (最新の点) が widthStart、age=0 (最古) が widthEnd。
    EXPECT_NEAR(scene::TrailWidthAt(trail, 1.0f, 1.0f), 0.20f, testkit::kTolerance);
    EXPECT_NEAR(scene::TrailWidthAt(trail, 0.0f, 0.0f), 0.02f, testkit::kTolerance);
    EXPECT_NEAR(scene::TrailWidthAt(trail, 0.5f, 0.5f), 0.11f, testkit::kTolerance);
}

TEST_F(TrailMultiKeyTest, KeepsTheTwoEndpointsWhenTheCurveHasTooFewKeys)
{
    scene::TrailComponent trail;
    trail.widthStart = 0.20f;
    trail.widthEnd   = 0.02f;
    trail.widthCurveEnabled = true;
    trail.widthCurve.keyCount = 1;

    EXPECT_FALSE(scene::TrailUsesWidthCurve(trail));
    EXPECT_NEAR(scene::TrailWidthAt(trail, 0.5f, 0.5f), 0.11f, testkit::kTolerance);
}

TEST_F(TrailMultiKeyTest, ScalesTheCurveByWidthStart)
{
    scene::TrailComponent trail;
    trail.widthStart = 0.40f;
    /// @note カーブが有効な間は使われない
    trail.widthEnd   = 0.02f;
    trail.widthCurveEnabled = true;
    trail.widthCurve = Ramp();

    ASSERT_TRUE(scene::TrailUsesWidthCurve(trail));
    EXPECT_NEAR(scene::TrailWidthAt(trail, 0.0f, 0.0f), 0.0f, testkit::kTolerance);
    EXPECT_NEAR(scene::TrailWidthAt(trail, 0.5f, 0.0f), 0.20f, testkit::kTolerance);
    EXPECT_NEAR(scene::TrailWidthAt(trail, 1.0f, 0.0f), 0.40f, testkit::kTolerance);
}

TEST_F(TrailMultiKeyTest, IgnoresTheEasedAgeWhileTheCurveIsUsed)
{
    scene::TrailComponent trail;
    trail.widthStart = 1.0f;
    trail.widthCurveEnabled = true;
    trail.widthCurve = Ramp();

    /// @note イージングを二重に掛けると «カーブどおりの形» にならない。
    EXPECT_NEAR(scene::TrailWidthAt(trail, 0.25f, 0.9f),
                scene::TrailWidthAt(trail, 0.25f, 0.1f), testkit::kTolerance);
}

/// @name 多キーの色

TEST_F(TrailMultiKeyTest, LeavesTheColorGradientOffByDefault)
{
    const scene::TrailComponent trail;

    EXPECT_FALSE(trail.colorGradientEnabled);
    EXPECT_FALSE(scene::TrailUsesColorGradient(trail));
}

TEST_F(TrailMultiKeyTest, NeedsTwoKeysBeforeTheGradientTakesOver)
{
    scene::TrailComponent trail;
    trail.colorGradientEnabled = true;
    trail.colorGradient.keyCount = 1;
    EXPECT_FALSE(scene::TrailUsesColorGradient(trail));

    trail.colorGradient.keyCount = 2;
    EXPECT_TRUE(scene::TrailUsesColorGradient(trail));
}

/// @name 画面効果の既定

TEST_F(TrailMultiKeyTest, LeavesTheShockRingDisabledByDefault)
{
    const renderer::PostProcessSettings settings;

    /// @note 振幅 0 でシェーダー側が素通しする。半径や幅が入っていても絵は変わらない。
    EXPECT_EQ(settings.lens.shockRingAmplitude, 0.0f);
    EXPECT_EQ(settings.exposure, 1.0f);
    EXPECT_EQ(settings.colorGrading.contrast, 0.0f);
    EXPECT_EQ(settings.colorGrading.saturation, 1.0f);
}

} // namespace fbzz::tests
