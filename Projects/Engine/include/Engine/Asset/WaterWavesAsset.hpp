/// @file    WaterWavesAsset.hpp
/// @brief   水面で共有する、決定的な Gerstner 波の設定。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Asset/DataAsset.hpp>
#include <Fluid/GerstnerWaves.hpp>
#include <array>
#include <cstdint>

namespace fbzz::asset {

/// @brief シードで生成する基底波の設定を .fzdata に保存する。
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

    /// @brief アセットの設定から基底波を生成する。設定が無効な場合は出力を変更しない。
    /// @param out 最大 16 本の基底波の出力先。
    /// @return アルゴリズムの版、数値の範囲、波の本数のいずれかが無効なら false。
    [[nodiscard]] FBZZ_ENGINE_API bool GenerateWaves(
        std::array<fluid::GerstnerWave, fluid::MAX_BASE_GERSTNER_WAVES>& out) const;

    /// @brief 反射で公開する編集値を、数式だけを扱う生成器の設定へ変換する。
    [[nodiscard]] FBZZ_ENGINE_API fluid::GerstnerWaveGenerationSettings GenerationSettings() const;
};

} /// @note namespace fbzz::asset
