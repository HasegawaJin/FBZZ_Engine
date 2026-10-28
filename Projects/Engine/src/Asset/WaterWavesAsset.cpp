/// @file    WaterWavesAsset.cpp
/// @brief   Reflection and seeded generation for WaterWavesAsset.
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <Engine/Asset/WaterWavesAsset.hpp>

#include <Engine/Asset/DataAssetFactory.hpp>
#include <cmath>

namespace fbzz::asset {

void WaterWavesAsset::Reflect(scene::IReflector& reflector)
{
    reflector.Field("algorithmVersion", algorithmVersion);
    reflector.Field("seed", seed);
    reflector.Field("waveCount", waveCount);
    reflector.Field("minimumWavelength", minimumWavelength);
    reflector.Field("maximumWavelength", maximumWavelength);
    reflector.Field("minimumAmplitude", minimumAmplitude);
    reflector.Field("maximumAmplitude", maximumAmplitude);
    reflector.Field("directionCenterRadians", directionCenterRadians);
    reflector.Field("directionSpreadRadians", directionSpreadRadians);
    reflector.Field("minimumSteepness", minimumSteepness);
    reflector.Field("maximumSteepness", maximumSteepness);
    reflector.Field("minimumPhaseOffsetRadians", minimumPhaseOffsetRadians);
    reflector.Field("maximumPhaseOffsetRadians", maximumPhaseOffsetRadians);
    reflector.Field("waveGrouping", waveGrouping);
    reflector.Field("waveSpread", waveSpread);
}

bool WaterWavesAsset::GenerateWaves(
    std::array<fluid::GerstnerWave, fluid::MAX_BASE_GERSTNER_WAVES>& out) const
{
    if (!std::isfinite(waveGrouping) || waveGrouping < 0.0f || waveGrouping > 1.0f
        || !std::isfinite(waveSpread) || waveSpread < 0.0f || waveSpread > 1.0f) {
        return false;
    }
    return fluid::GenerateGerstnerWaves(GenerationSettings(), out);
}

fluid::GerstnerWaveGenerationSettings WaterWavesAsset::GenerationSettings() const
{
    fluid::GerstnerWaveGenerationSettings settings;
    settings.algorithmVersion = static_cast<std::uint32_t>(algorithmVersion);
    settings.seed = static_cast<std::uint32_t>(seed);
    settings.waveCount = static_cast<std::uint32_t>(waveCount);
    settings.minimumWavelength = minimumWavelength;
    settings.maximumWavelength = maximumWavelength;
    settings.minimumAmplitude = minimumAmplitude;
    settings.maximumAmplitude = maximumAmplitude;
    settings.directionCenterRadians = directionCenterRadians;
    settings.directionSpreadRadians = directionSpreadRadians;
    settings.minimumSteepness = minimumSteepness;
    settings.maximumSteepness = maximumSteepness;
    settings.minimumPhaseOffsetRadians = minimumPhaseOffsetRadians;
    settings.maximumPhaseOffsetRadians = maximumPhaseOffsetRadians;
    return settings;
}

} /// @note namespace fbzz::asset

FBZZ_REGISTER_BUILTIN_DATA_ASSET(::fbzz::asset::WaterWavesAsset);
