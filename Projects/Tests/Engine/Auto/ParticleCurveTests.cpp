/// @file    ParticleCurveTests.cpp
/// @brief   VFX の軽量カーブ / グラデーションの評価規則と、CPU 側の色空間変換を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// このカーブは CPU と GPU (ParticleGpuSim.cs.hlsl) の両方に同じ式で実装されている。
/// 片方だけ直すと «CPU 経路のエフェクトだけ色や大きさが違う» という、
/// 見比べないと気づけない壊れ方をする。CPU 側の答えを数値で固定しておく。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Scene/ParticleCurve.hpp>

#include <Math/Vector4.hpp>

namespace fbzz::tests {
namespace {

/// 0 → 1 へ立ち上がるだけの 2 キーのカーブ。
scene::ParticleCurve Ramp(scene::ParticleCurveInterpolation mode
                          = scene::ParticleCurveInterpolation::Linear)
{
    scene::ParticleCurve curve;
    curve.keys[0]       = { 0.0f, 0.0f };
    curve.keys[1]       = { 1.0f, 1.0f };
    curve.keyCount      = 2;
    curve.interpolation = mode;
    return curve;
}

} // namespace

class ParticleCurveTest : public testkit::EngineFixture {};

// --- 端点と範囲外 -----------------------------------------------------------

TEST_F(ParticleCurveTest, ReturnsTheEndpointsAtBothEnds)
{
    const scene::ParticleCurve curve = Ramp();

    EXPECT_NEAR(curve.Evaluate(0.0f), 0.0f, testkit::kTolerance);
    EXPECT_NEAR(curve.Evaluate(1.0f), 1.0f, testkit::kTolerance);
}

TEST_F(ParticleCurveTest, ClampsOutsideTheKeyRange)
{
    // 寿命 0..1 の外へ出る呼び方は普通に起きる (寿命が伸びる演出)。外挿しない。
    const scene::ParticleCurve curve = Ramp();

    EXPECT_NEAR(curve.Evaluate(-1.0f), 0.0f, testkit::kTolerance);
    EXPECT_NEAR(curve.Evaluate(2.0f), 1.0f, testkit::kTolerance);
}

TEST_F(ParticleCurveTest, ASingleKeyIsConstant)
{
    scene::ParticleCurve curve;
    curve.keys[0]  = { 0.5f, 0.75f };
    curve.keyCount = 1;

    EXPECT_NEAR(curve.Evaluate(0.0f), 0.75f, testkit::kTolerance);
    EXPECT_NEAR(curve.Evaluate(1.0f), 0.75f, testkit::kTolerance);
}

TEST_F(ParticleCurveTest, TreatsAZeroKeyCountAsOne)
{
    // 壊れたアセットや初期化途中で 0 が来る。0 除算や範囲外参照へ落ちないこと。
    scene::ParticleCurve curve;
    curve.keyCount = 0;

    EXPECT_NEAR(curve.Evaluate(0.5f), curve.keys[0].value, testkit::kTolerance);
}

TEST_F(ParticleCurveTest, IgnoresAKeyCountBeyondTheCapacity)
{
    scene::ParticleCurve curve = Ramp();
    curve.keyCount = scene::kMaxParticleCurveKeys + 10;

    EXPECT_NEAR(curve.Evaluate(1.0f), curve.keys[scene::kMaxParticleCurveKeys - 1].value,
                testkit::kTolerance);
}

// --- 補間モード -------------------------------------------------------------

TEST_F(ParticleCurveTest, LinearInterpolatesEvenly)
{
    const scene::ParticleCurve curve = Ramp();

    EXPECT_NEAR(curve.Evaluate(0.25f), 0.25f, testkit::kTolerance);
    EXPECT_NEAR(curve.Evaluate(0.5f), 0.5f, testkit::kTolerance);
}

TEST_F(ParticleCurveTest, StepHoldsThePreviousValue)
{
    // フリップブックの段階切替や点滅。区間の途中で中間値を作ってはいけない。
    const scene::ParticleCurve curve = Ramp(scene::ParticleCurveInterpolation::Step);

    EXPECT_NEAR(curve.Evaluate(0.5f), 0.0f, testkit::kTolerance);
    EXPECT_NEAR(curve.Evaluate(0.99f), 0.0f, testkit::kTolerance);
}

TEST_F(ParticleCurveTest, StepSwitchesExactlyOnTheNextKey)
{
    // 区間は [前のキー, 次のキー)。次のキーへ到達した時点で切り替わる。
    // 越えたときにしか切り替わらないと、寿命 0..1 のカーブでは t=1.0 が
    // 最後のフレームなので、最終キー (フリップブックの最終コマ・消え際の点滅) が
    // 一度も表示されないまま粒子が死ぬ。
    const scene::ParticleCurve curve = Ramp(scene::ParticleCurveInterpolation::Step);

    EXPECT_NEAR(curve.Evaluate(0.999f), 0.0f, testkit::kTolerance);
    EXPECT_NEAR(curve.Evaluate(1.0f), 1.0f, testkit::kTolerance);
}

TEST_F(ParticleCurveTest, StepShowsEveryKeyOfAFlipbook)
{
    // 4 コマのフリップブック。どのコマも «自分のキー時刻ちょうど» で出ること。
    scene::ParticleCurve curve;
    curve.keys[0]       = { 0.0f, 0.0f };
    curve.keys[1]       = { 0.25f, 1.0f };
    curve.keys[2]       = { 0.5f, 2.0f };
    curve.keys[3]       = { 1.0f, 3.0f };
    curve.keyCount      = 4;
    curve.interpolation = scene::ParticleCurveInterpolation::Step;

    EXPECT_NEAR(curve.Evaluate(0.0f), 0.0f, testkit::kTolerance);
    EXPECT_NEAR(curve.Evaluate(0.25f), 1.0f, testkit::kTolerance);
    EXPECT_NEAR(curve.Evaluate(0.5f), 2.0f, testkit::kTolerance);
    EXPECT_NEAR(curve.Evaluate(1.0f), 3.0f, testkit::kTolerance);
    // 区間の途中では前のコマのまま。中間値は作らない。
    EXPECT_NEAR(curve.Evaluate(0.4f), 1.0f, testkit::kTolerance);
}

TEST_F(ParticleCurveTest, SmoothEasesInAndOutButKeepsTheMidpoint)
{
    const scene::ParticleCurve curve = Ramp(scene::ParticleCurveInterpolation::Smooth);

    EXPECT_NEAR(curve.Evaluate(0.5f), 0.5f, testkit::kTolerance);
    EXPECT_LT(curve.Evaluate(0.25f), 0.25f);   // 立ち上がりは遅い
    EXPECT_GT(curve.Evaluate(0.75f), 0.75f);   // 終わりは速く詰める
}

TEST_F(ParticleCurveTest, SmoothStaysInsideTheKeyValues)
{
    // smoothstep は行き過ぎない。1 を超えると加算パーティクルが白飛びする。
    const scene::ParticleCurve curve = Ramp(scene::ParticleCurveInterpolation::Smooth);

    for (int step = 0; step <= 20; ++step) {
        const float t = static_cast<float>(step) / 20.0f;

        EXPECT_GE(curve.Evaluate(t), -testkit::kTolerance);
        EXPECT_LE(curve.Evaluate(t), 1.0f + testkit::kTolerance);
    }
}

TEST_F(ParticleCurveTest, InterpolationHelperMatchesEachMode)
{
    // GPU 側 (ParticleGpuSim.cs.hlsl の ApplyCurveInterpolation) と同じ式であることを、
    // 係数そのもので固定する。片方だけ直すと «CPU では正しいが GPU では違う» になる。
    EXPECT_NEAR(scene::ApplyCurveInterpolation(0.3f, scene::ParticleCurveInterpolation::Linear),
                0.3f, testkit::kTolerance);
    EXPECT_NEAR(scene::ApplyCurveInterpolation(0.5f, scene::ParticleCurveInterpolation::Smooth),
                0.5f, testkit::kTolerance);

    // Step は区間の途中では 0、次のキーへ到達した時点で 1。
    EXPECT_NEAR(scene::ApplyCurveInterpolation(0.0f, scene::ParticleCurveInterpolation::Step),
                0.0f, testkit::kTolerance);
    EXPECT_NEAR(scene::ApplyCurveInterpolation(0.3f, scene::ParticleCurveInterpolation::Step),
                0.0f, testkit::kTolerance);
    EXPECT_NEAR(scene::ApplyCurveInterpolation(0.999f, scene::ParticleCurveInterpolation::Step),
                0.0f, testkit::kTolerance);
    EXPECT_NEAR(scene::ApplyCurveInterpolation(1.0f, scene::ParticleCurveInterpolation::Step),
                1.0f, testkit::kTolerance);
}

// --- 複数キー ---------------------------------------------------------------

TEST_F(ParticleCurveTest, PicksTheRightSegmentAmongManyKeys)
{
    // 「立ち上がり → 保持 → 減衰」。区間の選び方が 1 つずれると保持が消える。
    scene::ParticleCurve curve;
    curve.keys[0]  = { 0.0f, 0.0f };
    curve.keys[1]  = { 0.2f, 1.0f };
    curve.keys[2]  = { 0.8f, 1.0f };
    curve.keys[3]  = { 1.0f, 0.0f };
    curve.keyCount = 4;

    EXPECT_NEAR(curve.Evaluate(0.1f), 0.5f, testkit::kTolerance);
    EXPECT_NEAR(curve.Evaluate(0.5f), 1.0f, testkit::kTolerance);
    EXPECT_NEAR(curve.Evaluate(0.9f), 0.5f, testkit::kTolerance);
}

TEST_F(ParticleCurveTest, SurvivesTwoKeysAtTheSameTime)
{
    // Editor でキーを重ねると起きる。0 除算で NaN を出さないこと。
    scene::ParticleCurve curve;
    curve.keys[0]  = { 0.0f, 0.0f };
    curve.keys[1]  = { 0.5f, 1.0f };
    curve.keys[2]  = { 0.5f, 0.0f };
    curve.keyCount = 3;

    const float value = curve.Evaluate(0.5f);

    EXPECT_GE(value, 0.0f);
    EXPECT_LE(value, 1.0f);
}

// --- グラデーション ---------------------------------------------------------

TEST_F(ParticleCurveTest, GradientInterpolatesEveryChannel)
{
    scene::ParticleGradient gradient;
    gradient.keys[0]  = { 0.0f, { 1.0f, 0.0f, 0.0f, 1.0f } };
    gradient.keys[1]  = { 1.0f, { 0.0f, 1.0f, 0.0f, 0.0f } };
    gradient.keyCount = 2;

    const math::Vector4 mid = gradient.Evaluate(0.5f);

    EXPECT_VEC4_NEAR(mid, math::Vector4(0.5f, 0.5f, 0.0f, 0.5f), testkit::kLooseTolerance);
}

TEST_F(ParticleCurveTest, GradientClampsOutsideTheKeyRange)
{
    scene::ParticleGradient gradient;
    gradient.keys[0]  = { 0.0f, { 1.0f, 0.0f, 0.0f, 1.0f } };
    gradient.keys[1]  = { 1.0f, { 0.0f, 0.0f, 1.0f, 1.0f } };
    gradient.keyCount = 2;

    EXPECT_VEC4_NEAR(gradient.Evaluate(-1.0f), math::Vector4(1.0f, 0.0f, 0.0f, 1.0f),
                     testkit::kTolerance);
    EXPECT_VEC4_NEAR(gradient.Evaluate(5.0f), math::Vector4(0.0f, 0.0f, 1.0f, 1.0f),
                     testkit::kTolerance);
}

TEST_F(ParticleCurveTest, GradientStepSwitchesExactlyOnTheNextKey)
{
    // 色も同じ規則。「炎から煙へ切り替わる瞬間」を作るのが Step の用途なので、
    // 切り替え先の色がその時刻に出ないと意味が無い。
    scene::ParticleGradient gradient;
    gradient.keys[0]       = { 0.0f, { 1.0f, 0.0f, 0.0f, 1.0f } };
    gradient.keys[1]       = { 0.5f, { 0.0f, 1.0f, 0.0f, 1.0f } };
    gradient.keyCount      = 2;
    gradient.interpolation = scene::ParticleCurveInterpolation::Step;

    EXPECT_VEC4_NEAR(gradient.Evaluate(0.4f), math::Vector4(1.0f, 0.0f, 0.0f, 1.0f),
                     testkit::kLooseTolerance);
    EXPECT_VEC4_NEAR(gradient.Evaluate(0.5f), math::Vector4(0.0f, 1.0f, 0.0f, 1.0f),
                     testkit::kLooseTolerance);
}

TEST_F(ParticleCurveTest, LinearisedGradientIsDarkerThanTheAuthoredColour)
{
    // キーは «カラーピッカーに見える値» (sRGB)。シェーダーへ渡る直前に一度だけ
    // リニアへ落とす。二重に掛けても掛け忘れても、絵の明るさが変わる。
    scene::ParticleGradient gradient;
    gradient.keys[0]  = { 0.0f, { 0.5f, 0.5f, 0.5f, 1.0f } };
    gradient.keys[1]  = { 1.0f, { 0.5f, 0.5f, 0.5f, 1.0f } };
    gradient.keyCount = 2;

    const math::Vector4 gamma  = gradient.Evaluate(0.5f);
    const math::Vector4 linear = gradient.EvaluateLinear(0.5f);

    EXPECT_LT(linear.x, gamma.x);
    EXPECT_NEAR(linear.w, gamma.w, testkit::kTolerance);   // アルファは変換しない
}

} // namespace fbzz::tests
