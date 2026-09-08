/// @file    ParticleCodecTests.cpp
/// @brief   カーブ / グラデーションの TOML 往復が値を落とさないことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// エフェクトの見た目は .mat と .particle に散らばった数十項目で決まる。
/// 往復で 1 項目でも落ちると、開き直したときにその項目だけ既定値へ戻る ──
/// 「昨日いじった炎が今日は違う」という、履歴からも追えない壊れ方をする。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Asset/ParticleEmitterAssetCodec.hpp>
#include <Engine/Scene/ParticleCurve.hpp>

#include <toml++/toml.hpp>

namespace fbzz::tests {
namespace {

/// 既定値と全部違うカーブ。«書き忘れた項目» を既定値との一致で炙り出す。
scene::ParticleCurve DistinctCurve()
{
    scene::ParticleCurve curve;
    curve.keys[0]       = { 0.0f, 0.25f };
    curve.keys[1]       = { 0.3f, 1.5f };
    curve.keys[2]       = { 0.7f, 0.75f };
    curve.keys[3]       = { 1.0f, 0.0f };
    curve.keyCount      = 4;
    curve.interpolation = scene::ParticleCurveInterpolation::Smooth;
    return curve;
}

scene::ParticleGradient DistinctGradient()
{
    scene::ParticleGradient gradient;
    gradient.keys[0]     = { 0.0f, { 2.0f, 0.5f, 0.25f, 1.0f } };   // HDR の RGB
    gradient.keys[1]     = { 0.6f, { 1.0f, 1.0f, 0.0f, 0.5f } };
    gradient.keys[2]     = { 1.0f, { 0.0f, 0.0f, 0.0f, 0.0f } };
    gradient.keyCount    = 3;
    gradient.interpolation = scene::ParticleCurveInterpolation::Step;
    return gradient;
}

/// 1 つのテーブルへ書き出し、同じキーで読み戻す。
scene::ParticleCurve RoundTrip(const scene::ParticleCurve& curve)
{
    toml::table root;
    root.insert("curve", asset::SerializeParticleCurve(curve));

    scene::ParticleCurve restored;
    asset::DeserializeParticleCurve(root, "curve", restored);
    return restored;
}

scene::ParticleGradient RoundTrip(const scene::ParticleGradient& gradient)
{
    toml::table root;
    root.insert("gradient", asset::SerializeParticleGradient(gradient));

    scene::ParticleGradient restored;
    asset::DeserializeParticleGradient(root, "gradient", restored);
    return restored;
}

} // namespace

class ParticleCodecTest : public testkit::EngineFixture {};

// --- カーブ -----------------------------------------------------------------

TEST_F(ParticleCodecTest, CurveKeepsItsKeysThroughARoundTrip)
{
    const scene::ParticleCurve original = DistinctCurve();

    const scene::ParticleCurve restored = RoundTrip(original);

    ASSERT_EQ(restored.keyCount, original.keyCount);
    for (std::uint32_t i = 0; i < original.keyCount; ++i) {
        EXPECT_NEAR(restored.keys[i].time, original.keys[i].time, testkit::kTolerance) << i;
        EXPECT_NEAR(restored.keys[i].value, original.keys[i].value, testkit::kTolerance) << i;
    }
}

TEST_F(ParticleCodecTest, CurveKeepsItsInterpolationMode)
{
    // モードを落とすと «なめらかにしたカーブが折れ線に戻る»。値は合っているので気づけない。
    const scene::ParticleCurve restored = RoundTrip(DistinctCurve());

    EXPECT_EQ(restored.interpolation, scene::ParticleCurveInterpolation::Smooth);
}

TEST_F(ParticleCodecTest, CurveEvaluatesTheSameAfterARoundTrip)
{
    // 最終的に一致していてほしいのは «同じ時刻で同じ値» であること。
    const scene::ParticleCurve original = DistinctCurve();
    const scene::ParticleCurve restored = RoundTrip(original);

    for (int step = 0; step <= 20; ++step) {
        const float t = static_cast<float>(step) / 20.0f;

        EXPECT_NEAR(restored.Evaluate(t), original.Evaluate(t), testkit::kLooseTolerance)
            << "t=" << t;
    }
}

TEST_F(ParticleCodecTest, DeserialisingAMissingKeyLeavesTheTargetAlone)
{
    // 古い .particle には新しい項目が無い。読めなくなるのではなく既定値のまま残す。
    const toml::table empty;
    scene::ParticleCurve curve = DistinctCurve();

    asset::DeserializeParticleCurve(empty, "curve", curve);

    EXPECT_EQ(curve.keyCount, 4u);
    EXPECT_EQ(curve.interpolation, scene::ParticleCurveInterpolation::Smooth);
}

// --- グラデーション ---------------------------------------------------------

TEST_F(ParticleCodecTest, GradientKeepsEveryChannelThroughARoundTrip)
{
    const scene::ParticleGradient original = DistinctGradient();

    const scene::ParticleGradient restored = RoundTrip(original);

    ASSERT_EQ(restored.keyCount, original.keyCount);
    for (std::uint32_t i = 0; i < original.keyCount; ++i) {
        EXPECT_NEAR(restored.keys[i].time, original.keys[i].time, testkit::kTolerance) << i;
        EXPECT_VEC4_NEAR(restored.keys[i].color, original.keys[i].color,
                         testkit::kLooseTolerance)
            << i;
    }
}

TEST_F(ParticleCodecTest, GradientKeepsHdrValuesAboveOne)
{
    // RGB は 1 を超えてよい (HDR)。保存時にクランプすると発光が死ぬ。
    const scene::ParticleGradient restored = RoundTrip(DistinctGradient());

    EXPECT_GT(restored.keys[0].color.x, 1.0f);
}

TEST_F(ParticleCodecTest, GradientKeepsItsInterpolationMode)
{
    const scene::ParticleGradient restored = RoundTrip(DistinctGradient());

    EXPECT_EQ(restored.interpolation, scene::ParticleCurveInterpolation::Step);
}

TEST_F(ParticleCodecTest, GradientEvaluatesTheSameAfterARoundTrip)
{
    const scene::ParticleGradient original = DistinctGradient();
    const scene::ParticleGradient restored = RoundTrip(original);

    for (int step = 0; step <= 20; ++step) {
        const float t = static_cast<float>(step) / 20.0f;

        EXPECT_VEC4_NEAR(restored.Evaluate(t), original.Evaluate(t), testkit::kLooseTolerance)
            << "t=" << t;
    }
}

TEST_F(ParticleCodecTest, ADefaultCurveSurvivesTheRoundTripUnchanged)
{
    // 既定値そのものが往復で変わると、触っていないエフェクトの差分が出る。
    const scene::ParticleCurve original;

    const scene::ParticleCurve restored = RoundTrip(original);

    EXPECT_EQ(restored.keyCount, original.keyCount);
    EXPECT_EQ(restored.interpolation, original.interpolation);
    EXPECT_NEAR(restored.Evaluate(0.5f), original.Evaluate(0.5f), testkit::kTolerance);
}

} // namespace fbzz::tests
