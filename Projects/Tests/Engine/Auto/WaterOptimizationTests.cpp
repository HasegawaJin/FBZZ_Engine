/// @file    WaterOptimizationTests.cpp
/// @brief   波キャッシュと力場カリングの数値的な互換性を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include <TestKit/TestKit.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Fields/FlowFieldEval.hpp>
#include <Engine/Scene/Transform.hpp>
#include <cmath>

namespace fbzz::tests {
namespace {

/// @note キャッシュ導入前の評価順序を固定する独立参照。最適化側の評価関数を呼ばない。
float OriginalHeight(const scene::WaterComponent& water, float x, float z, float time)
{
    using Water = scene::WaterComponent;
    auto evaluate = [&](math::Vector2 point, bool horizontal) {
        math::Vector3 result{};
        for (int i = 0; i < 4; ++i) {
            const auto& wave = water.waves[static_cast<size_t>(i)];
            if (wave.amplitude < 0.0001f || wave.wavelength <= 0.0001f) continue;
            const float fade = Water::WaveMeshFade(wave.wavelength, water.cellSize);
            if (fade <= 0.0f) continue;
            const float k = math::TWO_PI / wave.wavelength;
            const float omega = std::sqrt(9.8f * k);
            for (int component = 0; component < 2; ++component) {
                const auto part = Water::WaveSpreadComponent(i, component, wave.direction.Normalized(), water.waveSpread);
                if (part.amplitudeScale <= 0.001f) continue;
                const auto dir = part.direction;
                const float envelope = Water::WaveGroupEnvelope(i, dir, k, omega, point.x, point.y, time, water.waveGrouping);
                const float phase = k * (dir.x * point.x + dir.y * point.y) - omega * time;
                if (horizontal) {
                    const float push = math::Clamp01(wave.steepness) * wave.amplitude * fade
                        * part.amplitudeScale * envelope * std::cos(phase);
                    result.x += dir.x * push;
                    result.z += dir.y * push;
                } else {
                    result.y += wave.amplitude * fade * part.amplitudeScale * envelope * std::sin(phase);
                }
            }
        }
        return result;
    };
    const math::Vector2 target{ x, z };
    auto point = target;
    const float peak = (1.0f + math::Clamp01(water.waveGrouping)) * Water::WaveSpreadAmplitudeSum(water.waveSpread);
    float contraction = 0.0f;
    for (const auto& wave : water.waves) {
        if (wave.amplitude < 0.0001f || wave.wavelength <= 0.0001f) continue;
        contraction += math::Clamp01(wave.steepness) * wave.amplitude
            * Water::WaveMeshFade(wave.wavelength, water.cellSize) * peak * (math::TWO_PI / wave.wavelength);
    }
    if (contraction > 0.0001f) {
        const float relax = 1.0f / (std::max)(contraction, 1.0f);
        for (int i = 0; i < Water::SURFACE_SOLVE_ITERATIONS; ++i) {
            const auto offset = evaluate(point, true);
            point.x += (target.x - (point.x + offset.x)) * relax;
            point.y += (target.y - (point.y + offset.z)) * relax;
        }
    }
    return evaluate(point, false).y;
}

class WaterOptimizationTest : public testkit::Fixture {};
}

TEST_F(WaterOptimizationTest, CachedWavesMatchOriginalAndDetectDirectEdits)
{
    scene::WaterComponent water;
    for (int setting = 0; setting < 12; ++setting) {
        water.waveSpread = static_cast<float>(setting) / 11.0f;
        water.waveGrouping = static_cast<float>(setting % 4) / 3.0f;
        water.cellSize = { 0.3f * setting, 0.2f * setting };
        for (size_t i = 0; i < water.waves.size(); ++i) {
            auto& wave = water.waves[i];
            wave.direction = { static_cast<float>(i) - 1.5f, 0.2f + 0.1f * setting };
            wave.amplitude = 0.2f * (i + 1) * (setting + 1);
            wave.wavelength = 4.0f + 7.0f * i;
            wave.steepness = 0.1f * setting;
        }
        for (int point = 0; point < 15; ++point) {
            const float x = 13.3f * point - 100.0f;
            const float z = 5.1f * point - 30.0f;
            const float time = 0.73f * point;
            EXPECT_NEAR(water.GetSurfaceHeightAt(x, z, time), OriginalHeight(water, x, z, time), 1.0e-5f);
            EXPECT_LE(std::abs(water.GetSurfaceHeightAt(x, z, time)), water.SurfaceHeightBound());
        }
    }
    water.enableGerstnerWaves = false;
    EXPECT_FLOAT_EQ(water.GetSurfaceHeightAt(1.0f, 2.0f, 3.0f), 0.0f);
}

TEST_F(WaterOptimizationTest, BoundsContainFlowAndRippleDisplacement)
{
    scene::WaterComponent water;
    water.waveGrouping = 1.0f;
    water.waveSpread = 1.0f;
    water.surfaceFlowCount = 2;
    water.surfaceFlows[0].height = -3.0f;
    water.surfaceFlows[0].radius = 4.0f;
    water.surfaceFlows[1].height = 2.0f;
    water.surfaceFlows[1].radius = 7.0f;
    scene::WaterRipple ripple;
    ripple.amplitude = 1.0f;
    ripple.meshFade = 1.0f;
    ripple.radius = 2.0f;
    water.ripples.push_back(ripple);
    for (int i = -30; i <= 30; ++i)
        EXPECT_LE(std::abs(water.GetSurfaceHeightAt(i * 0.2f, 0.0f, i * 0.3f)), water.SurfaceHeightBound());
}

TEST_F(WaterOptimizationTest, CullingUsesLiveParticlePositionsAndKeepsBakedCoverage)
{
    scene::ParticleEmitter emitter;
    scene::Transform transform;
    emitter.settings.receiveFlowFields = true;
    emitter.settings.simulationSpace = scene::ParticleSimulationSpace::World;
    scene::Particle particle;
    particle.position = { 20.0f, 0.0f, 0.0f };
    emitter.runtime.particles.push_back(particle);
    scene::FlowFieldSettings settings;
    settings.fieldType = scene::FlowFieldType::Uniform;
    settings.radius = 2.0f;
    const auto near = scene::ResolveFlowField(settings, particle.position, {});
    const auto far = scene::ResolveFlowField(settings, { 100.0f, 0.0f, 0.0f }, {});
    settings.fieldType = scene::FlowFieldType::Baked;
    const auto baked = scene::ResolveFlowField(settings, { 100.0f, 0.0f, 0.0f }, {});
    std::vector<scene::ActiveFlowField> fields{ near, far, baked }, culled;
    scene::ResolveEmitterForces(emitter, transform, fields, culled);
    ASSERT_EQ(culled.size(), 2u);
    EXPECT_EQ(culled[0].position.x, 20.0f);
    bool originalCovered = false, culledCovered = false;
    const auto original = scene::SampleFlow(particle.position, fields, 0xFFFFFFFFu, 0.0f, &originalCovered);
    const auto optimized = scene::SampleFlow(particle.position, culled, 0xFFFFFFFFu, 0.0f, &culledCovered);
    EXPECT_VEC3_NEAR(original, optimized, testkit::kTolerance);
    EXPECT_EQ(originalCovered, culledCovered);
    const auto outside = scene::SampleFlow(math::Vector3::ZERO, fields, 0xFFFFFFFFu, 0.0f, &originalCovered);
    const auto outsideCulled = scene::SampleFlow(math::Vector3::ZERO, culled, 0xFFFFFFFFu, 0.0f, &culledCovered);
    EXPECT_VEC3_NEAR(outside, outsideCulled, testkit::kTolerance);
    EXPECT_TRUE(originalCovered);
    EXPECT_EQ(originalCovered, culledCovered);
}

TEST_F(WaterOptimizationTest, GpuBoundsAndRotatedBakedBoxCullConservatively)
{
    scene::ParticleEmitter emitter;
    scene::Transform transform;
    emitter.settings.receiveFlowFields = true;
    emitter.runtime.flowBoundsCenter = { 30.0f, 0.0f, 0.0f };
    emitter.runtime.flowBoundsRadius = 2.0f;
    scene::FlowFieldSettings settings;
    settings.radius = 1.0f;
    const auto near = scene::ResolveFlowField(settings, { 32.0f, 0.0f, 0.0f }, {});
    const auto far = scene::ResolveFlowField(settings, { 60.0f, 0.0f, 0.0f }, {});
    std::vector<scene::ActiveFlowField> fields{ near, far }, culled;
    scene::ResolveEmitterForces(emitter, transform, fields, culled, true);
    ASSERT_EQ(culled.size(), 1u);
    EXPECT_FLOAT_EQ(culled[0].position.x, 32.0f);
    settings.fieldType = scene::FlowFieldType::Baked;
    settings.vectorFieldExtents = { 1.0f, 2.0f, 6.0f };
    const auto rotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::PI * 0.5f);
    const auto baked = scene::ResolveFlowField(settings, math::Vector3::ZERO, rotation);
    EXPECT_TRUE(scene::FlowIntersectsSphere(baked, { 5.0f, 0.0f, 0.0f }, 0.1f));
    EXPECT_FALSE(scene::FlowIntersectsSphere(baked, { 0.0f, 0.0f, 5.0f }, 0.1f));
}

} // namespace fbzz::tests
