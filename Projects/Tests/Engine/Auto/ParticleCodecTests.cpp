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
#include <Engine/Scene/Components/ParticleEmitter.hpp>
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

// ── 内蔵の力 (localForces) ────────────────────────────────────────────────
//
// 重力・空気抵抗・乱流・周回・放射は個別フィールドから力場のリストへ移った。
// 旧いシーンと .particle にはまだ個別キーが残っているので、読み込みで力へ組み直す。
// ここが落ちると、既存のエフェクトが全部その場に浮く。

const scene::ParticleForceFieldSettings* FindForce(
    const scene::ParticleEmitterSettings& emitter, scene::ParticleForceFieldType type)
{
    return emitter.FindLocalForce(type);
}

TEST_F(ParticleCodecTest, LegacyGravityKeyBecomesAWindForce)
{
    toml::table legacy;
    legacy.insert("gravity", toml::array{ 0.0, -9.8, 0.0 });

    scene::ParticleEmitterSettings emitter;
    asset::DeserializeParticleEmitterSettings(legacy, emitter);

    const auto* wind = FindForce(emitter, scene::ParticleForceFieldType::Wind);
    ASSERT_NE(wind, nullptr);
    EXPECT_NEAR(wind->strength, 9.8f, testkit::kTolerance);
    EXPECT_VEC3_NEAR(wind->direction, math::Vector3(0.0f, -1.0f, 0.0f), testkit::kTolerance);
    // 半径 0 = 減衰なしで全体に効く。ここが 5 のままだと «エミッターから 5m 先で
    // 重力が消える» という、遠くの粒だけ浮く壊れ方をする。
    EXPECT_NEAR(wind->radius, 0.0f, testkit::kTolerance);
    EXPECT_VEC3_NEAR(emitter.GravityAcceleration(), math::Vector3(0.0f, -9.8f, 0.0f),
                     testkit::kTolerance);
}

TEST_F(ParticleCodecTest, LegacyMotionKeysBecomeTheMatchingForces)
{
    toml::table legacy;
    legacy.insert("gravity", toml::array{ 0.0, -5.0, 0.0 });
    legacy.insert("velocityDamping", 2.5);
    legacy.insert("noiseStrength", 3.0);
    legacy.insert("noiseFrequency", 1.25);
    legacy.insert("noiseSpeed", 2.0);
    legacy.insert("orbitalVelocity", 4.0);
    legacy.insert("orbitalAxis", toml::array{ 0.0, 0.0, 1.0 });
    legacy.insert("radialVelocity", -1.5);

    scene::ParticleEmitterSettings emitter;
    asset::DeserializeParticleEmitterSettings(legacy, emitter);

    const auto* drag = FindForce(emitter, scene::ParticleForceFieldType::Drag);
    ASSERT_NE(drag, nullptr);
    EXPECT_NEAR(drag->strength, 2.5f, testkit::kTolerance);

    const auto* turbulence = FindForce(emitter, scene::ParticleForceFieldType::Turbulence);
    ASSERT_NE(turbulence, nullptr);
    EXPECT_NEAR(turbulence->strength, 3.0f, testkit::kTolerance);
    EXPECT_NEAR(turbulence->noiseFrequency, 1.25f, testkit::kTolerance);
    EXPECT_NEAR(turbulence->noiseSpeed, 2.0f, testkit::kTolerance);

    const auto* vortex = FindForce(emitter, scene::ParticleForceFieldType::Vortex);
    ASSERT_NE(vortex, nullptr);
    EXPECT_NEAR(vortex->strength, 4.0f, testkit::kTolerance);
    EXPECT_VEC3_NEAR(vortex->direction, math::Vector3(0.0f, 0.0f, 1.0f), testkit::kTolerance);
    // 周回はエミッター原点まわりの運動。World にすると原点が世界の中心へ飛ぶ。
    EXPECT_EQ(vortex->space, scene::ParticleForceFieldSpace::Emitter);

    // 旧 radialVelocity の負値は «吸い込み»。Repulse の負の強さと同じ規約なので符号を保つ。
    const auto* repulse = FindForce(emitter, scene::ParticleForceFieldType::Repulse);
    ASSERT_NE(repulse, nullptr);
    EXPECT_NEAR(repulse->strength, -1.5f, testkit::kTolerance);
    EXPECT_EQ(repulse->space, scene::ParticleForceFieldSpace::Emitter);
}

TEST_F(ParticleCodecTest, LegacyFileWithoutMotionKeysGetsNoForces)
{
    // 旧ファイルが gravity を書いていない = 既定 (0,-5,0) だった、が正しい復元。
    // 新規エミッターの既定リストをそのまま残すと «書いていないのに力が増える» になる。
    toml::table legacy;
    legacy.insert("emitRate", 10.0);

    scene::ParticleEmitterSettings emitter;
    asset::DeserializeParticleEmitterSettings(legacy, emitter);

    EXPECT_EQ(emitter.localForces.size(), 1u); // 既定の重力 1 本だけ
    EXPECT_VEC3_NEAR(emitter.GravityAcceleration(), math::Vector3(0.0f, -5.0f, 0.0f),
                     testkit::kTolerance);
}

TEST_F(ParticleCodecTest, AnEmptyForceListSurvivesTheRoundTrip)
{
    // «力を全部消した» と «旧ファイル» は区別が要る。前者で既定の重力が復活すると、
    // 無重力に作ったエフェクトが開き直すたびに落ち始める。
    scene::ParticleEmitterSettings original;
    original.localForces.clear();

    scene::ParticleEmitterSettings restored;
    asset::DeserializeParticleEmitterSettings(
        asset::SerializeParticleEmitterSettings(original), restored);

    EXPECT_TRUE(restored.localForces.empty());
}

TEST_F(ParticleCodecTest, AVectorFieldForceSurvivesTheRoundTrip)
{
    scene::ParticleEmitterSettings original;
    original.localForces.clear();
    scene::ParticleForceFieldSettings field;
    field.fieldType            = scene::ParticleForceFieldType::VectorField;
    field.space                = scene::ParticleForceFieldSpace::Emitter;
    field.strength             = 3.5f;
    field.vectorFieldPath      = "Assets/VFX/Tornado.vfield";
    field.vectorFieldExtents   = { 2.0f, 8.0f, 2.0f };
    field.vectorFieldTightness = 0.75f;
    original.localForces.push_back(field);

    scene::ParticleEmitterSettings restored;
    asset::DeserializeParticleEmitterSettings(
        asset::SerializeParticleEmitterSettings(original), restored);

    ASSERT_EQ(restored.localForces.size(), 1u);
    const auto& r = restored.localForces[0];
    EXPECT_EQ(r.fieldType, scene::ParticleForceFieldType::VectorField);
    EXPECT_EQ(r.space, scene::ParticleForceFieldSpace::Emitter);
    EXPECT_NEAR(r.strength, 3.5f, testkit::kTolerance);
    EXPECT_EQ(r.vectorFieldPath, "Assets/VFX/Tornado.vfield");
    EXPECT_VEC3_NEAR(r.vectorFieldExtents, math::Vector3(2.0f, 8.0f, 2.0f), testkit::kTolerance);
    EXPECT_NEAR(r.vectorFieldTightness, 0.75f, testkit::kTolerance);
}

} // namespace fbzz::tests
