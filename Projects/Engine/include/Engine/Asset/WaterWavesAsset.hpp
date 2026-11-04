/// @file    WaterWavesAsset.hpp
/// @brief   Shared deterministic Gerstner wave authoring for water surfaces.
/// @author  Hasegawa Jin
/// @date    2026-10-07
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Asset/DataAsset.hpp>
#include <Fluid/GerstnerWaves.hpp>
#include <array>
#include <cstdint>

namespace fbzz::asset {

/// @brief Seeded base-wave settings stored in a native .fzdata asset.
/// @see Docs/design/water-waves.md 水面の波
class WaterWavesAsset final : public DataAsset {
public:
    static constexpr const char* TYPE_NAME = "WaterWavesAsset";

    const char* GetTypeName() const override { return TYPE_NAME; }
    FBZZ_ENGINE_API void Reflect(scene::IReflector& reflector) override;

    int algorithmVersion = static_cast<int>(fluid::GERSTNER_WAVE_GENERATION_ALGORITHM_VERSION);
    int seed = 0x6D2B79F5;
    int waveCount = 8;
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
    float waveGrouping = 0.45f;
    float waveSpread = 0.0f;

    /// @brief Generate the asset's authored base waves without changing output on invalid settings.
    /// @param out Destination for up to 16 generated base waves.
    /// @return False when the algorithm version, numeric ranges, or count is invalid.
    [[nodiscard]] FBZZ_ENGINE_API bool GenerateWaves(
        std::array<fluid::GerstnerWave, fluid::MAX_BASE_GERSTNER_WAVES>& out) const;

    /// @brief Convert reflected authoring values to the math-only generator settings.
    [[nodiscard]] FBZZ_ENGINE_API fluid::GerstnerWaveGenerationSettings GenerationSettings() const;
};

} /// @note namespace fbzz::asset
