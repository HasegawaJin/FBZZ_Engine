/// @file    GerstnerWaves.hpp
/// @brief   Deterministic Gerstner wave generation and contraction bounds.
/// @author  Hasegawa Jin
/// @date    2026-10-07
#pragma once
#include <Fluid/Api.hpp>

#include <Math/Vector2.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fbzz::fluid {

inline constexpr std::uint32_t GERSTNER_WAVE_GENERATION_ALGORITHM_VERSION = 1;
inline constexpr std::size_t MAX_BASE_GERSTNER_WAVES = 16;

/// @brief One analytic Gerstner wave in world XZ coordinates.
struct GerstnerWave {
    math::Vector2 direction = { 1.0f, 0.0f };
    float amplitude = 0.30f;
    float wavelength = 10.0f;
    float steepness = 0.50f;
    /// @brief Additive phase [rad] used by both the carrier and its group envelope.
    /// @see https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-1-effective-water-simulation-physical-models GPU Gems 1.2, Gerstner wave model.
    float phaseOffsetRadians = 0.0f;
};

/// @brief Seeded inputs for variable-count Gerstner wave generation.
struct GerstnerWaveGenerationSettings {
    std::uint32_t algorithmVersion = GERSTNER_WAVE_GENERATION_ALGORITHM_VERSION;
    std::uint32_t seed = 0x6D2B79F5u;
    std::uint32_t waveCount = 8;
    float minimumWavelength = 4.0f;
    float maximumWavelength = 48.0f;
    float minimumAmplitude = 0.08f;
    float maximumAmplitude = 0.45f;
    float directionCenterRadians = 0.0f;
    float directionSpreadRadians = 0.8f;
    float minimumSteepness = 0.15f;
    float maximumSteepness = 0.55f;
    float minimumPhaseOffsetRadians = 0.0f;
    float maximumPhaseOffsetRadians = 6.2831853071795864769f;

    bool operator==(const GerstnerWaveGenerationSettings&) const = default;
};

/// @brief Generate a stable wave set from integer-seeded samples.
/// @param settings Validated ranges and algorithm version.
/// @param out Replaced only when generation succeeds.
/// @return False for unsupported versions, invalid ranges, or counts above 16.
/// @see https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-1-effective-water-simulation-physical-models GPU Gems 1.2, Gerstner wave model.
/// @see https://www.pcg-random.org/paper.html Melissa E. O'Neill, PCG: A Family of Simple Fast Space-Efficient Statistically Good Algorithms for Random Number Generation, 2014.
[[nodiscard]] FBZZ_FLUID_API bool GenerateGerstnerWaves(
    const GerstnerWaveGenerationSettings& settings,
    std::array<GerstnerWave, MAX_BASE_GERSTNER_WAVES>& out);

/// @brief Return an upper bound for the horizontal Gerstner contraction, including group-envelope gradients.
/// @param waves Active base waves only.
/// @param grouping Envelope depth in [0,1].
/// @param spread Directional spread in [0,1].
/// @return A finite bound, or infinity when an input is invalid.
/// @see https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-1-effective-water-simulation-physical-models GPU Gems 1.2.2, mapping Gerstner displacement back to the surface.
[[nodiscard]] FBZZ_FLUID_API float GerstnerWaveContractionBound(
    std::span<const GerstnerWave> waves, float grouping, float spread);

/// @brief Scale steepness uniformly until the supplied contraction limit is met.
/// @param waves Active base waves only; invalid input leaves all waves unchanged.
/// @param grouping Envelope depth in [0,1].
/// @param spread Directional spread in [0,1].
/// @param maximumContraction Positive upper bound for inverse mapping.
/// @return False for invalid inputs; otherwise true, including an unchanged set.
/// @see https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-1-effective-water-simulation-physical-models GPU Gems 1.2.2, mapping Gerstner displacement back to the surface.
[[nodiscard]] FBZZ_FLUID_API bool LimitGerstnerWaveContraction(
    std::span<GerstnerWave> waves, float grouping, float spread, float maximumContraction);

} /// @note namespace fbzz::fluid
