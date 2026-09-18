/// @file    ParticleCodecTests.cpp
/// @brief   カーブ / グラデーションの TOML 往復が値を落とさないことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// @brief エフェクトの見た目は .mat と .particle に散らばった数十項目で決まる。
/// @brief 往復で 1 項目でも落ちると、開き直したときにその項目だけ既定値へ戻る ──
/// @brief 「昨日いじった炎が今日は違う」という、履歴からも追えない壊れ方をする。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Asset/ParticleEmitterAssetCodec.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/ParticleCurve.hpp>

#include <toml++/toml.hpp>

namespace fbzz::tests {
namespace {

/// @brief 既定値と全部違うカーブ。«書き忘れた項目» を既定値との一致で炙り出す。
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
    /// @note HDR の RGB
    gradient.keys[0]     = { 0.0f, { 2.0f, 0.5f, 0.25f, 1.0f } };
    gradient.keys[1]     = { 0.6f, { 1.0f, 1.0f, 0.0f, 0.5f } };
    gradient.keys[2]     = { 1.0f, { 0.0f, 0.0f, 0.0f, 0.0f } };
    gradient.keyCount    = 3;
    gradient.interpolation = scene::ParticleCurveInterpolation::Step;
    return gradient;
}

/// @brief 1 つのテーブルへ書き出し、同じキーで読み戻す。
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

/// @name カーブ

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
    /// @note モードを落とすと «なめらかにしたカーブが折れ線に戻る»。値は合っているので気づけない。
    const scene::ParticleCurve restored = RoundTrip(DistinctCurve());

    EXPECT_EQ(restored.interpolation, scene::ParticleCurveInterpolation::Smooth);
}

TEST_F(ParticleCodecTest, CurveEvaluatesTheSameAfterARoundTrip)
{
    /// @note 最終的に一致していてほしいのは «同じ時刻で同じ値» であること。
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
    /// @note 古い .particle には新しい項目が無い。読めなくなるのではなく既定値のまま残す。
    const toml::table empty;
    scene::ParticleCurve curve = DistinctCurve();

    asset::DeserializeParticleCurve(empty, "curve", curve);

    EXPECT_EQ(curve.keyCount, 4u);
    EXPECT_EQ(curve.interpolation, scene::ParticleCurveInterpolation::Smooth);
}

/// @name グラデーション

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
    /// @note RGB は 1 を超えてよい (HDR)。保存時にクランプすると発光が死ぬ。
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
    /// @note 既定値そのものが往復で変わると、触っていないエフェクトの差分が出る。
    const scene::ParticleCurve original;

    const scene::ParticleCurve restored = RoundTrip(original);

    EXPECT_EQ(restored.keyCount, original.keyCount);
    EXPECT_EQ(restored.interpolation, original.interpolation);
    EXPECT_NEAR(restored.Evaluate(0.5f), original.Evaluate(0.5f), testkit::kTolerance);
}

/// @name 内蔵の流れ (localForces)
/// @brief 乱流・周回・放射は個別フィールドから流れのリストへ移り、さらに 2026-09-16 に
/// @brief 単位が加速度 [m/s^2] から流速 [m/s] へ変わった。重力と空気抵抗は «媒質の運動ではない»
/// @brief ので gravity / flowCoupling としてリストの外に出ている。
/// @brief ここが落ちると、既存のエフェクトが全部その場に浮くか、逆に一斉に減速する。

const scene::FlowFieldSettings* FindForce(
    const scene::ParticleEmitterSettings& emitter, scene::FlowFieldType type)
{
    return emitter.FindLocalForce(type);
}

TEST_F(ParticleCodecTest, LegacyGravityKeyBecomesTheEmitterGravity)
{
    toml::table legacy;
    legacy.insert("gravity", toml::array{ 0.0, -9.8, 0.0 });

    scene::ParticleEmitterSettings emitter;
    asset::DeserializeParticleEmitterSettings(legacy, emitter);

    /// @note 重力は加速度のまま。場に相乗りしていたのをエミッターの枠へ戻すだけで換算は無い。
    EXPECT_VEC3_NEAR(emitter.gravity, math::Vector3(0.0f, -9.8f, 0.0f), testkit::kTolerance);
    EXPECT_EQ(FindForce(emitter, scene::FlowFieldType::Uniform), nullptr)
        << "重力が流れとして残ると、環境流と二重に掛かる";
}

TEST_F(ParticleCodecTest, LegacyMotionKeysBecomeTheMatchingFlows)
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

    /// @note 旧 velocityDamping [1/s] と新 flowCoupling [1/s] は同じ単位。換算しない。
    EXPECT_NEAR(emitter.flowCoupling, 2.5f, testkit::kTolerance);
    EXPECT_VEC3_NEAR(emitter.gravity, math::Vector3(0.0f, -5.0f, 0.0f), testkit::kTolerance);

    const float toFlow = scene::kLegacyAccelerationToFlowSpeed;

    const auto* curl = FindForce(emitter, scene::FlowFieldType::Curl);
    ASSERT_NE(curl, nullptr);
    EXPECT_NEAR(curl->strength, 3.0f * toFlow, testkit::kTolerance);
    EXPECT_NEAR(curl->noiseFrequency, 1.25f, testkit::kTolerance);
    EXPECT_NEAR(curl->noiseSpeed, 2.0f, testkit::kTolerance);

    const auto* vortex = FindForce(emitter, scene::FlowFieldType::Vortex);
    ASSERT_NE(vortex, nullptr);
    EXPECT_NEAR(vortex->strength, 4.0f * toFlow, testkit::kTolerance);
    EXPECT_VEC3_NEAR(vortex->direction, math::Vector3(0.0f, 0.0f, 1.0f), testkit::kTolerance);
    /// @note 周回はエミッター原点まわりの運動。World にすると原点が世界の中心へ飛ぶ。
    EXPECT_EQ(vortex->space, scene::FlowFieldSpace::Emitter);

    /// @note 旧 radialVelocity の負値は «吸い込み»。Source の負の強さと同じ規約なので符号を保つ。
    const auto* source = FindForce(emitter, scene::FlowFieldType::Source);
    ASSERT_NE(source, nullptr);
    EXPECT_NEAR(source->strength, -1.5f * toFlow, testkit::kTolerance);
    EXPECT_EQ(source->space, scene::FlowFieldSpace::Emitter);
}

TEST_F(ParticleCodecTest, LegacyFileWithoutMotionKeysGetsNoFlows)
{
    /// @note 旧ファイルが gravity を書いていない = 既定 (0,-5,0) だった、が正しい復元。
    toml::table legacy;
    legacy.insert("emitRate", 10.0);

    scene::ParticleEmitterSettings emitter;
    asset::DeserializeParticleEmitterSettings(legacy, emitter);

    EXPECT_TRUE(emitter.localForces.empty());
    EXPECT_VEC3_NEAR(emitter.gravity, scene::kDefaultParticleGravity, testkit::kTolerance);
}

/// @brief 2026-09-11〜09-16 の形式 (localForces はあるが flowCoupling が無い)。
/// @brief 下向きの Uniform は重力、Drag は結合係数、それ以外は加速度 → 流速。
TEST_F(ParticleCodecTest, LegacyAccelerationForcesBecomeFlowSpeeds)
{
    toml::table wind;
    wind.insert("fieldType", static_cast<std::int64_t>(scene::FlowFieldType::Uniform));
    wind.insert("space", static_cast<std::int64_t>(scene::FlowFieldSpace::World));
    wind.insert("direction", toml::array{ 0.0, -1.0, 0.0 });
    wind.insert("strength", 5.0);
    wind.insert("radius", 0.0);

    toml::table drag;
    drag.insert("fieldType", static_cast<std::int64_t>(scene::FlowFieldType::LegacyDrag));
    drag.insert("strength", 2.0);

    toml::table breeze;
    breeze.insert("fieldType", static_cast<std::int64_t>(scene::FlowFieldType::Uniform));
    breeze.insert("space", static_cast<std::int64_t>(scene::FlowFieldSpace::World));
    breeze.insert("direction", toml::array{ 1.0, 0.0, 0.0 });
    breeze.insert("strength", 10.0);
    breeze.insert("radius", 3.0);

    toml::table legacy;
    legacy.insert("localForces", toml::array{ wind, drag, breeze });

    scene::ParticleEmitterSettings emitter;
    asset::DeserializeParticleEmitterSettings(legacy, emitter);

    /// @note 真下を向いた World の Uniform は重力へ。残るのは横向きの 1 本だけ。
    EXPECT_VEC3_NEAR(emitter.gravity, math::Vector3(0.0f, -5.0f, 0.0f), testkit::kTolerance);
    EXPECT_NEAR(emitter.flowCoupling, 2.0f, testkit::kTolerance);
    ASSERT_EQ(emitter.localForces.size(), 1u);
    EXPECT_EQ(emitter.localForces[0].fieldType, scene::FlowFieldType::Uniform);
    EXPECT_NEAR(emitter.localForces[0].strength,
                10.0f * scene::kLegacyAccelerationToFlowSpeed, testkit::kTolerance);
}

TEST_F(ParticleCodecTest, AnEmptyFlowListSurvivesTheRoundTrip)
{
    /// @note «流れを全部消した» と «旧ファイル» は区別が要る。前者で既定が復活すると、
    ///       意図して止めたエフェクトが開き直すたびに動き始める。
    scene::ParticleEmitterSettings original;
    original.localForces.clear();
    original.flowCoupling = 1.25f;
    original.gravity = { 0.0f, 0.0f, 0.0f };

    scene::ParticleEmitterSettings restored;
    asset::DeserializeParticleEmitterSettings(
        asset::SerializeParticleEmitterSettings(original), restored);

    EXPECT_TRUE(restored.localForces.empty());
    EXPECT_NEAR(restored.flowCoupling, 1.25f, testkit::kTolerance);
    EXPECT_VEC3_NEAR(restored.gravity, math::Vector3::ZERO, testkit::kTolerance);
}

TEST_F(ParticleCodecTest, ABakedFlowSurvivesTheRoundTrip)
{
    scene::ParticleEmitterSettings original;
    original.localForces.clear();
    scene::FlowFieldSettings field;
    field.fieldType          = scene::FlowFieldType::Baked;
    field.space              = scene::FlowFieldSpace::Emitter;
    field.strength           = 3.5f;
    field.vectorFieldPath    = "Assets/VFX/Tornado_Velocity.png";
    field.vectorFieldExtents = { 2.0f, 8.0f, 2.0f };
    original.localForces.push_back(field);

    scene::ParticleEmitterSettings restored;
    asset::DeserializeParticleEmitterSettings(
        asset::SerializeParticleEmitterSettings(original), restored);

    ASSERT_EQ(restored.localForces.size(), 1u);
    const auto& r = restored.localForces[0];
    EXPECT_EQ(r.fieldType, scene::FlowFieldType::Baked);
    EXPECT_EQ(r.space, scene::FlowFieldSpace::Emitter);
    EXPECT_NEAR(r.strength, 3.5f, testkit::kTolerance);
    EXPECT_EQ(r.vectorFieldPath, "Assets/VFX/Tornado_Velocity.png");
    EXPECT_VEC3_NEAR(r.vectorFieldExtents, math::Vector3(2.0f, 8.0f, 2.0f), testkit::kTolerance);
}

} // namespace fbzz::tests
