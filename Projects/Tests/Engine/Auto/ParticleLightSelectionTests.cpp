/// @file    ParticleLightSelectionTests.cpp
/// @brief   Lights モジュールの粒子選択 (上限・明るさ順・決定論・割合) を固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// ライト配列の枠は LightComponent と共有なので、選び方が揺れると «シーンの別の場所に
/// 粒子を置いたら松明が消えた» が起きる。選び方は純関数にしてここで守る。

#include <TestKit/TestKit.hpp>

#include <Engine/Scene/Components/ParticleLightSelection.hpp>

namespace fbzz::tests {
namespace {

scene::Particle MakeParticle(float alpha, float seed, float size = 1.0f)
{
    scene::Particle particle{};
    particle.position = { seed, 0.0f, 0.0f };
    particle.color = { 1.0f, 0.5f, 0.25f, alpha };
    particle.size = size;
    particle.age = 0.0f;
    particle.lifetime = 1.0f;
    particle.spriteSeed = seed;
    return particle;
}

scene::ParticleLightSettings EnabledLight()
{
    scene::ParticleLightSettings light;
    light.lightEnabled = true;
    light.lightMaxCount = 8;
    light.lightFadeWithAlpha = true;
    return light;
}

} // namespace

TEST(ParticleLightSelectionTest, DisabledModuleEmitsNothing)
{
    std::vector<scene::Particle> particles = { MakeParticle(1.0f, 0.1f) };
    scene::ParticleLightSettings light = EnabledLight();
    light.lightEnabled = false;
    std::vector<scene::ParticleLightEmission> out;
    scene::SelectParticleLights(light, particles, 16, out);
    EXPECT_TRUE(out.empty());
}

TEST(ParticleLightSelectionTest, KeepsTheBrightestWithinMaxCountAndBudget)
{
    std::vector<scene::Particle> particles;
    for (int i = 0; i < 10; ++i) particles.push_back(MakeParticle(0.1f * static_cast<float>(i + 1), 0.05f * i));
    scene::ParticleLightSettings light = EnabledLight();
    light.lightMaxCount = 4;

    std::vector<scene::ParticleLightEmission> out;
    scene::SelectParticleLights(light, particles, 16, out);
    ASSERT_EQ(out.size(), 4u);
    /// @note アルファで強さが落ちるので、アルファの大きい順 (= 配列の後ろから) に並ぶ。
    EXPECT_NEAR(out[0].intensity, 1.0f, 1.0e-5f);
    EXPECT_NEAR(out[1].intensity, 0.9f, 1.0e-5f);
    EXPECT_NEAR(out[3].intensity, 0.7f, 1.0e-5f);

    /// @note 残り枠が上限より少なければ、そちらで打ち切る。
    scene::SelectParticleLights(light, particles, 2, out);
    EXPECT_EQ(out.size(), 2u);
}

TEST(ParticleLightSelectionTest, EqualBrightnessIsBrokenByParticleOrder)
{
    std::vector<scene::Particle> particles = { MakeParticle(1.0f, 0.3f), MakeParticle(1.0f, 0.1f),
                                               MakeParticle(1.0f, 0.2f) };
    scene::ParticleLightSettings light = EnabledLight();
    light.lightMaxCount = 2;
    std::vector<scene::ParticleLightEmission> out;
    scene::SelectParticleLights(light, particles, 16, out);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_NEAR(out[0].position.x, 0.3f, 1.0e-6f);
    EXPECT_NEAR(out[1].position.x, 0.1f, 1.0e-6f);
}

TEST(ParticleLightSelectionTest, ColorAndRangeFollowTheSettings)
{
    std::vector<scene::Particle> particles = { MakeParticle(1.0f, 0.4f, 3.0f) };
    scene::ParticleLightSettings light = EnabledLight();
    light.lightColor = { 2.0f, 2.0f, 2.0f, 1.0f };
    light.lightRange = 1.5f;
    light.lightRangeFromSize = true;

    std::vector<scene::ParticleLightEmission> out;
    scene::SelectParticleLights(light, particles, 16, out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_NEAR(out[0].color.x, 2.0f, 1.0e-6f);
    EXPECT_NEAR(out[0].color.y, 1.0f, 1.0e-6f);
    EXPECT_NEAR(out[0].color.z, 0.5f, 1.0e-6f);
    EXPECT_NEAR(out[0].range, 4.5f, 1.0e-5f);

    light.lightUseParticleColor = false;
    scene::SelectParticleLights(light, particles, 16, out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_NEAR(out[0].color.z, 2.0f, 1.0e-6f);
}

TEST(ParticleLightSelectionTest, RatioIsStablePerParticleAndRoughlyProportional)
{
    int passed = 0;
    for (int i = 0; i < 1000; ++i) {
        const float seed = static_cast<float>(i) / 1000.0f;
        const bool first = scene::ParticlePassesLightRatio(seed, 0.25f);
        EXPECT_EQ(first, scene::ParticlePassesLightRatio(seed, 0.25f));
        if (first) ++passed;
    }
    EXPECT_GT(passed, 200);
    EXPECT_LT(passed, 300);
    EXPECT_TRUE(scene::ParticlePassesLightRatio(0.5f, 1.0f));
    EXPECT_FALSE(scene::ParticlePassesLightRatio(0.5f, 0.0f));
}

TEST(ParticleLightSelectionTest, DeadAndInvisibleParticlesDoNotShine)
{
    scene::Particle dead = MakeParticle(1.0f, 0.1f);
    dead.age = 2.0f;
    std::vector<scene::Particle> particles = { dead, MakeParticle(0.0f, 0.2f) };
    std::vector<scene::ParticleLightEmission> out;
    scene::SelectParticleLights(EnabledLight(), particles, 16, out);
    EXPECT_TRUE(out.empty());
}

} // namespace fbzz::tests
