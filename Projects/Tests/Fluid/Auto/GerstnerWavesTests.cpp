/// @file    GerstnerWavesTests.cpp
/// @brief   Deterministic wave generation and inverse-mapping contraction bounds.
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <TestKit/TestKit.hpp>
#include <Fluid/GerstnerWaves.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <span>

namespace fbzz::tests {
namespace {

bool SameWave(const fluid::GerstnerWave& a, const fluid::GerstnerWave& b)
{
    return a.direction.x == b.direction.x && a.direction.y == b.direction.y
        && a.amplitude == b.amplitude && a.wavelength == b.wavelength
        && a.steepness == b.steepness && a.phaseOffsetRadians == b.phaseOffsetRadians;
}

} /// @note namespace

class GerstnerWavesTest : public testkit::Fixture {};

TEST_F(GerstnerWavesTest, FixedSeedProducesTheSameVariableCountWaveSet)
{
    fluid::GerstnerWaveGenerationSettings settings;
    settings.waveCount = 12;
    std::array<fluid::GerstnerWave, fluid::MAX_BASE_GERSTNER_WAVES> first{};
    std::array<fluid::GerstnerWave, fluid::MAX_BASE_GERSTNER_WAVES> second{};
    ASSERT_TRUE(fluid::GenerateGerstnerWaves(settings, first));
    ASSERT_TRUE(fluid::GenerateGerstnerWaves(settings, second));
    for (size_t i = 0; i < settings.waveCount; ++i) EXPECT_TRUE(SameWave(first[i], second[i]));
    settings.seed += 1u;
    ASSERT_TRUE(fluid::GenerateGerstnerWaves(settings, second));
    EXPECT_FALSE(SameWave(first[0], second[0]));
}

TEST_F(GerstnerWavesTest, GeneratedCountAndConfiguredRangesAreHonored)
{
    fluid::GerstnerWaveGenerationSettings settings;
    settings.waveCount = 7;
    settings.minimumWavelength = settings.maximumWavelength = 19.0f;
    settings.minimumAmplitude = settings.maximumAmplitude = 0.27f;
    settings.directionCenterRadians = 0.6f;
    settings.directionSpreadRadians = 0.0f;
    settings.minimumSteepness = settings.maximumSteepness = 0.35f;
    settings.minimumPhaseOffsetRadians = settings.maximumPhaseOffsetRadians = 1.2f;
    std::array<fluid::GerstnerWave, fluid::MAX_BASE_GERSTNER_WAVES> waves{};
    ASSERT_TRUE(fluid::GenerateGerstnerWaves(settings, waves));
    for (size_t i = 0; i < settings.waveCount; ++i) {
        EXPECT_FLOAT_EQ(waves[i].wavelength, 19.0f);
        EXPECT_FLOAT_EQ(waves[i].amplitude, 0.27f);
        EXPECT_FLOAT_EQ(waves[i].steepness, 0.35f);
        EXPECT_FLOAT_EQ(waves[i].phaseOffsetRadians, 1.2f);
        EXPECT_NEAR(waves[i].direction.x, std::cos(0.6f), 1.0e-6f);
        EXPECT_NEAR(waves[i].direction.y, std::sin(0.6f), 1.0e-6f);
    }
}

TEST_F(GerstnerWavesTest, InvalidGenerationSettingsLeaveTheOutputUnchanged)
{
    std::array<fluid::GerstnerWave, fluid::MAX_BASE_GERSTNER_WAVES> waves{};
    waves[0] = {{0.25f, 0.75f}, 3.0f, 17.0f, 0.2f, -0.4f};
    const auto before = waves;
    fluid::GerstnerWaveGenerationSettings settings;
    settings.algorithmVersion += 1u;
    EXPECT_FALSE(fluid::GenerateGerstnerWaves(settings, waves));
    for (size_t i = 0; i < waves.size(); ++i) EXPECT_TRUE(SameWave(waves[i], before[i]));
    settings.algorithmVersion = fluid::GERSTNER_WAVE_GENERATION_ALGORITHM_VERSION;
    settings.waveCount = static_cast<std::uint32_t>(fluid::MAX_BASE_GERSTNER_WAVES + 1);
    EXPECT_FALSE(fluid::GenerateGerstnerWaves(settings, waves));
    for (size_t i = 0; i < waves.size(); ++i) EXPECT_TRUE(SameWave(waves[i], before[i]));
}

TEST_F(GerstnerWavesTest, LimiterBoundsScaledGroupedSpreadWavesAndRejectsInvalidInputAtomically)
{
    fluid::GerstnerWaveGenerationSettings settings;
    settings.waveCount = 8;
    settings.minimumAmplitude = 0.5f;
    settings.maximumAmplitude = 1.0f;
    std::array<fluid::GerstnerWave, fluid::MAX_BASE_GERSTNER_WAVES> waves{};
    ASSERT_TRUE(fluid::GenerateGerstnerWaves(settings, waves));
    for (size_t i = 0; i < settings.waveCount; ++i) waves[i].amplitude *= 3.0f;
    const std::span active(waves.data(), settings.waveCount);
    const float ungroupedBound = fluid::GerstnerWaveContractionBound(active, 0.0f, 0.0f);
    const float groupedSpreadBound = fluid::GerstnerWaveContractionBound(active, 0.45f, 0.8f);
    EXPECT_GT(groupedSpreadBound, ungroupedBound);
    ASSERT_TRUE(fluid::LimitGerstnerWaveContraction(
        std::span<fluid::GerstnerWave>(waves.data(), settings.waveCount), 0.45f, 0.8f, 0.95f));
    EXPECT_LE(fluid::GerstnerWaveContractionBound(active, 0.45f, 0.8f), 0.95f);

    waves[0].direction = math::Vector2::ZERO;
    const auto before = waves;
    EXPECT_FALSE(fluid::LimitGerstnerWaveContraction(
        std::span<fluid::GerstnerWave>(waves.data(), settings.waveCount), 0.45f, 0.8f, 0.95f));
    for (size_t i = 0; i < waves.size(); ++i) EXPECT_TRUE(SameWave(waves[i], before[i]));
}

} /// @note namespace fbzz::tests
