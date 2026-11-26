/// @file    GerstnerWaves.hpp
/// @brief   同じ設定から同じ水面の波を生成し、横ずれの変化率の上限を求める。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#pragma once
#include <Fluid/Api.hpp>

#include <Math/LinearAlgebra/Vector2.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fbzz::fluid {

inline constexpr std::uint32_t GERSTNER_WAVE_GENERATION_ALGORITHM_VERSION = 1;
inline constexpr std::size_t MAX_BASE_GERSTNER_WAVES = 16;

/// @brief 山と谷の上下動に水平方向のずれを加える、ワールド XZ 平面上の波 1 本。
struct GerstnerWave {
    math::Vector2 direction = { 1.0f, 0.0f }; ///< ワールド XZ 平面で山が進む向き。生成時は (cosθ, sinθ) で長さ 1 のベクトルにする。
    float amplitude = 0.30f; ///< 波の平均の高さから山頂までの高さ（メートル）。
    float wavelength = 10.0f; ///< 隣り合う山と山の間隔（メートル）。
    float steepness = 0.50f; ///< 波の横ずれの強さ（0～1）。同じ振幅でも値を上げるほど山が鋭くなる。
    /// @brief 波と、波の強弱を決める包絡線に共通して加える開始位相（ラジアン）。
    /// @see https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-1-effective-water-simulation-physical-models GPU Gems 1.2, Gerstner wave model.
    float phaseOffsetRadians = 0.0f; ///< XZ 原点での位相のオフセット [rad]。
};

/// @brief 本数と各値の範囲を決め、同じ方式・種・設定から同じ波の組を再現する。
struct GerstnerWaveGenerationSettings {
    std::uint32_t algorithmVersion = GERSTNER_WAVE_GENERATION_ALGORITHM_VERSION; ///< 保存した種をどの手順で波に変えるかを識別し、対応していない版の再生成を防ぐ。
    std::uint32_t seed = 0x6D2B79F5u; ///< 波長・振幅・方向・急峻度・位相を引くときの乱数シード。
    std::uint32_t waveCount = 8; ///< 出力配列の先頭から生成する波の本数。
    float minimumWavelength = 4.0f; ///< 生成する波長の下限 [m]。
    float maximumWavelength = 48.0f; ///< 生成する波長の上限 [m]。
    float minimumAmplitude = 0.08f; ///< 生成する波の片振幅の下限 [m]。
    float maximumAmplitude = 0.45f; ///< 生成する波の片振幅の上限 [m]。
    float directionCenterRadians = 0.0f; ///< 波向きの中心角。0 はワールドの +X 方向で、正の角度は +Z 側へ回る。
    float directionSpreadRadians = 0.8f; ///< 波向きを振り分ける角度の全幅。中心から左右にこの半分ずつ広がる。
    float minimumSteepness = 0.15f; ///< 生成する急峻度の下限。範囲は 0～1。
    float maximumSteepness = 0.55f; ///< 生成する急峻度の上限。範囲は 0～1。
    float minimumPhaseOffsetRadians = 0.0f; ///< 生成する位相オフセットの下限 [rad]。
    float maximumPhaseOffsetRadians = 6.2831853071795864769f; ///< 生成する位相オフセットの上限 [rad]。

    bool operator==(const GerstnerWaveGenerationSettings&) const = default;
};

/// @brief 乱数の種と指定範囲から、再現可能な波の組を生成する。
/// @param[in] settings 生成する本数、各値の範囲、生成方式の版。
/// @param[out] out 成功時だけ結果で全体を置き換える。先頭 waveCount 本が生成した波。
/// @return 方式の版や値の範囲、本数が無効なら false。
/// @see https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-1-effective-water-simulation-physical-models GPU Gems 1.2, Gerstner wave model.
/// @see https://www.pcg-random.org/paper.html Melissa E. O'Neill, PCG: A Family of Simple Fast Space-Efficient Statistically Good Algorithms for Random Number Generation, 2014.
[[nodiscard]] FBZZ_FLUID_API bool GenerateGerstnerWaves(
    const GerstnerWaveGenerationSettings& settings,
    std::array<GerstnerWave, MAX_BASE_GERSTNER_WAVES>& out);

/// @brief 水面上を移動したときに横ずれがどれだけ変わるか、波の強弱の変化も含めた上限を求める。
/// @param[in] waves 重ね合わせる波だけを渡す。
/// @param[in] grouping 波の強弱をどの程度付けるかを表す 0～1 の値。
/// @param[in] spread 波を異なる方向へ分散させる度合い（0～1）。
/// @return 上限値。入力が無効か計算結果を表現できないときは正の無限大。
/// @see https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-1-effective-water-simulation-physical-models GPU Gems 1.2.2, mapping Gerstner displacement back to the surface.
[[nodiscard]] FBZZ_FLUID_API float GerstnerWaveContractionBound(
    std::span<const GerstnerWave> waves, float grouping, float spread);

/// @brief 横ずれの変化率が上限に収まるように、すべての波の鋭さを同じ割合で弱める。
/// @param[in,out] waves 重ね合わせる波だけを渡す。入力が無効なときはどの波にも書き込まない。
/// @param[in] grouping 波の強弱をどの程度付けるかを表す 0～1 の値。
/// @param[in] spread 波を異なる方向へ分散させる度合い（0～1）。
/// @param[in] maximumContraction 変形後の水面から元の位置を求める際に許容する、横ずれの変化率の上限（正の値）。
/// @return 入力が無効なら false。すでに上限内なら波をそのままにして true。
/// @see https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-1-effective-water-simulation-physical-models GPU Gems 1.2.2, mapping Gerstner displacement back to the surface.
[[nodiscard]] FBZZ_FLUID_API bool LimitGerstnerWaveContraction(
    std::span<GerstnerWave> waves, float grouping, float spread, float maximumContraction);

} /// @note namespace fbzz::fluid
