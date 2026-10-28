/// @file    GerstnerWaves.cpp
/// @brief   Deterministic Gerstner wave generation and contraction bounds.
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <Fluid/GerstnerWaves.hpp>

#include <Math/MathUtils.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace fbzz::fluid {
namespace {

class Pcg32 {
public:
    explicit Pcg32(std::uint32_t seed)
        : m_state(static_cast<std::uint64_t>(seed) + 0x853C49E6748FEA9BULL)
    {
        static_cast<void>(Next());
    }

    std::uint32_t Next()
    {
        const std::uint64_t previous = m_state;
        m_state = previous * 6364136223846793005ULL + 0xDA3E39CB94B95BDBULL;
        const std::uint32_t shifted = static_cast<std::uint32_t>(((previous >> 18u) ^ previous) >> 27u);
        const std::uint32_t rotation = static_cast<std::uint32_t>(previous >> 59u);
        return (shifted >> rotation) | (shifted << ((0u - rotation) & 31u));
    }

    float UnitFloat()
    {
        return static_cast<float>(Next() >> 8u) * (1.0f / 16777216.0f);
    }

private:
    std::uint64_t m_state;
};

bool IsFinite(float value)
{
    return std::isfinite(value);
}

bool ValidGenerationSettings(const GerstnerWaveGenerationSettings& settings)
{
    return settings.algorithmVersion == GERSTNER_WAVE_GENERATION_ALGORITHM_VERSION
        && settings.waveCount <= MAX_BASE_GERSTNER_WAVES
        && IsFinite(settings.minimumWavelength) && settings.minimumWavelength >= 0.01f
        && IsFinite(settings.maximumWavelength) && settings.maximumWavelength >= settings.minimumWavelength
        && settings.maximumWavelength <= 100000.0f
        && IsFinite(settings.minimumAmplitude) && settings.minimumAmplitude >= 0.0f
        && IsFinite(settings.maximumAmplitude) && settings.maximumAmplitude >= settings.minimumAmplitude
        && settings.maximumAmplitude <= 10000.0f
        && IsFinite(settings.directionCenterRadians)
        && std::abs(settings.directionCenterRadians) <= 1000000.0f
        && IsFinite(settings.directionSpreadRadians) && settings.directionSpreadRadians >= 0.0f
        && settings.directionSpreadRadians <= math::TWO_PI
        && IsFinite(settings.minimumSteepness) && settings.minimumSteepness >= 0.0f
        && IsFinite(settings.maximumSteepness) && settings.maximumSteepness <= 1.0f
        && settings.maximumSteepness >= settings.minimumSteepness
        && IsFinite(settings.minimumPhaseOffsetRadians)
        && IsFinite(settings.maximumPhaseOffsetRadians)
        && std::abs(settings.minimumPhaseOffsetRadians) <= 1000000.0f
        && std::abs(settings.maximumPhaseOffsetRadians) <= 1000000.0f
        && settings.maximumPhaseOffsetRadians >= settings.minimumPhaseOffsetRadians;
}

} /// @note namespace

bool GenerateGerstnerWaves(const GerstnerWaveGenerationSettings& settings,
                           std::array<GerstnerWave, MAX_BASE_GERSTNER_WAVES>& out)
{
    if (!ValidGenerationSettings(settings)) return false;

    std::array<GerstnerWave, MAX_BASE_GERSTNER_WAVES> generated{};
    Pcg32 random(settings.seed);
    for (std::size_t i = 0; i < settings.waveCount; ++i) {
        const float wavelengthT = random.UnitFloat();
        const float amplitudeT = random.UnitFloat();
        const float directionT = random.UnitFloat();
        const float steepnessT = random.UnitFloat();
        const float phaseT = random.UnitFloat();
        const float angle = settings.directionCenterRadians
            + (directionT - 0.5f) * settings.directionSpreadRadians;
        GerstnerWave wave;
        wave.direction = { std::cos(angle), std::sin(angle) };
        wave.wavelength = settings.minimumWavelength
            + (settings.maximumWavelength - settings.minimumWavelength) * wavelengthT;
        wave.amplitude = settings.minimumAmplitude
            + (settings.maximumAmplitude - settings.minimumAmplitude) * amplitudeT;
        wave.steepness = settings.minimumSteepness
            + (settings.maximumSteepness - settings.minimumSteepness) * steepnessT;
        wave.phaseOffsetRadians = settings.minimumPhaseOffsetRadians
            + (settings.maximumPhaseOffsetRadians - settings.minimumPhaseOffsetRadians) * phaseT;
        generated[i] = wave;
    }

    out = generated;
    return true;
}

float GerstnerWaveContractionBound(std::span<const GerstnerWave> waves, float grouping, float spread)
{
    if (!IsFinite(grouping) || grouping < 0.0f || grouping > 1.0f
        || !IsFinite(spread) || spread < 0.0f || spread > 1.0f) {
        return std::numeric_limits<float>::infinity();
    }

    double sum = 0.0;
    for (const GerstnerWave& wave : waves) {
        const float directionLengthSq = wave.direction.LengthSq();
        if (!IsFinite(wave.direction.x) || !IsFinite(wave.direction.y)
            || !IsFinite(directionLengthSq) || directionLengthSq <= 1.0e-8f
            || !IsFinite(wave.amplitude) || wave.amplitude < 0.0f
            || !IsFinite(wave.wavelength) || wave.wavelength <= 0.0f
            || !IsFinite(wave.steepness) || wave.steepness < 0.0f || wave.steepness > 1.0f
            || !IsFinite(wave.phaseOffsetRadians)) {
            return std::numeric_limits<float>::infinity();
        }
        const double waveNumber = static_cast<double>(math::TWO_PI) / wave.wavelength;
        sum += static_cast<double>(wave.steepness) * wave.amplitude * waveNumber;
    }

    const double spreadEnergy = 0.40 * spread;
    const double spreadSum = std::sqrt(1.0 - spreadEnergy) + std::sqrt(spreadEnergy);
    /// @note The envelope gradient and float-rounded directions add to the carrier-wave Jacobian bound.
    const double envelopeDerivative = 1.0 + grouping + grouping * 0.195;
    const double directionSlack = 1.0 + 32.0 * std::numeric_limits<float>::epsilon();
    const double bound = sum * envelopeDerivative * spreadSum * directionSlack;
    if (!std::isfinite(bound) || bound > std::numeric_limits<float>::max())
        return std::numeric_limits<float>::infinity();
    if (bound == 0.0) return 0.0f;
    const float roundedBound = static_cast<float>(bound);
    if (roundedBound == 0.0f) return std::numeric_limits<float>::denorm_min();
    return std::nextafter(roundedBound, std::numeric_limits<float>::infinity());
}

bool LimitGerstnerWaveContraction(std::span<GerstnerWave> waves, float grouping, float spread,
                                 float maximumContraction)
{
    if (!IsFinite(maximumContraction) || maximumContraction <= 0.0f) return false;
    const float bound = GerstnerWaveContractionBound(waves, grouping, spread);
    if (!IsFinite(bound)) return false;
    if (bound <= maximumContraction || bound <= 0.0f) return true;

    const float safeMaximum = maximumContraction
        * (1.0f - 64.0f * std::numeric_limits<float>::epsilon());
    const float scale = std::nextafter(safeMaximum / bound, 0.0f);
    for (GerstnerWave& wave : waves) wave.steepness *= scale;
    return true;
}

} /// @note namespace fbzz::fluid
