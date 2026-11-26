/// @file    GerstnerWaves.cpp
/// @brief   同じ種から再現できるゲルストナー波の生成と、水面の横ずれが変化する割合の上限。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <Fluid/Fields/GerstnerWaves.hpp>

#include <Math/Scalar/MathUtils.hpp>
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
        // float が正確に表せる上位 24 ビットだけを使い、2 の 24 乗で割って 0 以上 1 未満の乱数にする。
        return static_cast<float>(Next() >> 8u) * (1.0f / 16777216.0f);
    }

private:
    std::uint64_t m_state; ///< 次の PCG 出力を生成するための 64 ビットの内部状態。
};

bool IsFinite(float value)
{
    return std::isfinite(value);
}

bool ValidGenerationSettings(const GerstnerWaveGenerationSettings& settings)
{
    // 対応する生成規則と、出力配列に収まる本数を先に確かめる。
    if (settings.algorithmVersion != GERSTNER_WAVE_GENERATION_ALGORITHM_VERSION) {
        return false;
    }
    if (settings.waveCount > MAX_BASE_GERSTNER_WAVES) {
        return false;
    }

    // 波長と振幅は、補間する上下限を有限・昇順に揃える。
    const bool validWavelength = IsFinite(settings.minimumWavelength) && settings.minimumWavelength >= 0.01f
        && IsFinite(settings.maximumWavelength) && settings.maximumWavelength >= settings.minimumWavelength
        && settings.maximumWavelength <= 100000.0f;
    if (!validWavelength) {
        return false;
    }
    const bool validAmplitude = IsFinite(settings.minimumAmplitude) && settings.minimumAmplitude >= 0.0f
        && IsFinite(settings.maximumAmplitude) && settings.maximumAmplitude >= settings.minimumAmplitude
        && settings.maximumAmplitude <= 10000.0f;
    if (!validAmplitude) {
        return false;
    }

    // 方向の中心・幅、急峻度、位相を別々に検証し、乱数を引く前に範囲を確定する。
    const bool validDirection = IsFinite(settings.directionCenterRadians)
        && std::abs(settings.directionCenterRadians) <= 1000000.0f
        && IsFinite(settings.directionSpreadRadians) && settings.directionSpreadRadians >= 0.0f
        && settings.directionSpreadRadians <= math::TWO_PI;
    if (!validDirection) {
        return false;
    }
    const bool validSteepness = IsFinite(settings.minimumSteepness) && settings.minimumSteepness >= 0.0f
        && IsFinite(settings.maximumSteepness) && settings.maximumSteepness <= 1.0f
        && settings.maximumSteepness >= settings.minimumSteepness;
    if (!validSteepness) {
        return false;
    }
    return IsFinite(settings.minimumPhaseOffsetRadians)
        && IsFinite(settings.maximumPhaseOffsetRadians)
        && std::abs(settings.minimumPhaseOffsetRadians) <= 1000000.0f
        && std::abs(settings.maximumPhaseOffsetRadians) <= 1000000.0f
        && settings.maximumPhaseOffsetRadians >= settings.minimumPhaseOffsetRadians;
}

} /// @note namespace

bool GenerateGerstnerWaves(const GerstnerWaveGenerationSettings& settings,
                           std::array<GerstnerWave, MAX_BASE_GERSTNER_WAVES>& out)
{
    if (!ValidGenerationSettings(settings)) {
        return false;
    }

    // 同じ種と版から同じ列を生成できるよう、各波の乱数を決められた順に引く。
    std::array<GerstnerWave, MAX_BASE_GERSTNER_WAVES> generated{};
    Pcg32 random(settings.seed);
    for (std::size_t waveIndex = 0; waveIndex < settings.waveCount; ++waveIndex) {
        const float wavelengthFraction = random.UnitFloat();
        const float amplitudeFraction = random.UnitFloat();
        const float directionFraction = random.UnitFloat();
        const float steepnessFraction = random.UnitFloat();
        const float phaseFraction = random.UnitFloat();
        const float angle = settings.directionCenterRadians
            + (directionFraction - 0.5f) * settings.directionSpreadRadians;
        GerstnerWave wave;
        wave.direction = { std::cos(angle), std::sin(angle) };
        wave.wavelength = settings.minimumWavelength
            + (settings.maximumWavelength - settings.minimumWavelength) * wavelengthFraction;
        wave.amplitude = settings.minimumAmplitude
            + (settings.maximumAmplitude - settings.minimumAmplitude) * amplitudeFraction;
        wave.steepness = settings.minimumSteepness
            + (settings.maximumSteepness - settings.minimumSteepness) * steepnessFraction;
        wave.phaseOffsetRadians = settings.minimumPhaseOffsetRadians
            + (settings.maximumPhaseOffsetRadians - settings.minimumPhaseOffsetRadians) * phaseFraction;
        generated[waveIndex] = wave;
    }

    // 生成が完了してから出力配列全体を差し替える。
    out = generated;
    return true;
}

float GerstnerWaveContractionBound(std::span<const GerstnerWave> waves, float grouping, float spread)
{
    if (!IsFinite(grouping) || grouping < 0.0f || grouping > 1.0f) {
        return std::numeric_limits<float>::infinity();
    }
    if (!IsFinite(spread) || spread < 0.0f || spread > 1.0f) {
        return std::numeric_limits<float>::infinity();
    }

    double sum = 0.0;
    for (const GerstnerWave& wave : waves) {
        const float directionLengthSquared = wave.direction.LengthSq();
        const bool validDirection = IsFinite(wave.direction.x) && IsFinite(wave.direction.y)
            && IsFinite(directionLengthSquared) && directionLengthSquared > 1.0e-8f;
        if (!validDirection) {
            return std::numeric_limits<float>::infinity();
        }
        const bool validAmplitude = IsFinite(wave.amplitude) && wave.amplitude >= 0.0f;
        if (!validAmplitude) {
            return std::numeric_limits<float>::infinity();
        }
        const bool validWavelength = IsFinite(wave.wavelength) && wave.wavelength > 0.0f;
        if (!validWavelength) {
            return std::numeric_limits<float>::infinity();
        }
        const bool validSteepness = IsFinite(wave.steepness) && wave.steepness >= 0.0f && wave.steepness <= 1.0f;
        if (!validSteepness || !IsFinite(wave.phaseOffsetRadians)) {
            return std::numeric_limits<float>::infinity();
        }
        // 波長ぶん進むと位相が 1 周するので、1 メートル当たりの角度は 2π ÷ 波長。同じ高さの波なら短い波ほど傾きが大きい。
        const double waveNumber = static_cast<double>(math::TWO_PI) / wave.wavelength;
        sum += static_cast<double>(wave.steepness) * wave.amplitude * waveNumber;
    }

    const double spreadEnergy = 0.40 * spread;
    const double spreadSum = std::sqrt(1.0 - spreadEnergy) + std::sqrt(spreadEnergy);
    // 波の強弱が場所によって変わる分と、方向ベクトルの丸め誤差も加えて、横ずれの変化率を過小評価しないようにする。
    const double envelopeDerivative = 1.0 + grouping + grouping * 0.195;
    const double directionSlack = 1.0 + 32.0 * std::numeric_limits<float>::epsilon();
    const double bound = sum * envelopeDerivative * spreadSum * directionSlack;
    if (!std::isfinite(bound) || bound > std::numeric_limits<float>::max()) {
        return std::numeric_limits<float>::infinity();
    }
    if (bound == 0.0) {
        return 0.0f;
    }
    // 安全側の上限が float への丸めで小さくならないよう、正の値は大きい側の隣の表現値へ進める。
    const float roundedBound = static_cast<float>(bound);
    if (roundedBound == 0.0f) {
        return std::numeric_limits<float>::denorm_min();
    }
    return std::nextafter(roundedBound, std::numeric_limits<float>::infinity());
}

bool LimitGerstnerWaveContraction(std::span<GerstnerWave> waves, float grouping, float spread,
                                 float maximumContraction)
{
    if (!IsFinite(maximumContraction) || maximumContraction <= 0.0f) {
        return false;
    }
    const float bound = GerstnerWaveContractionBound(waves, grouping, spread);
    if (!IsFinite(bound)) {
        return false;
    }
    if (bound <= maximumContraction || bound <= 0.0f) {
        return true;
    }

    // 急峻度だけを一律に下げる。浮動小数点の丸めで上限を越えないよう比率を小さい側へ丸める。
    const float safeMaximum = maximumContraction
        * (1.0f - 64.0f * std::numeric_limits<float>::epsilon());
    const float scale = std::nextafter(safeMaximum / bound, 0.0f);
    for (GerstnerWave& wave : waves) {
        wave.steepness *= scale;
    }
    return true;
}

} /// @note namespace fbzz::fluid
