/// @file    FluidLiquidDomain.hpp
/// @brief   粒子の排出と近傍格子で共有する液体の計算領域。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#pragma once

namespace fbzz::fluid::liquid_domain {

/// @note 近傍格子が覆う範囲。これより外へ出た粒子は画面外なので捨てる。
inline constexpr float MIN_X = -1.6f;
inline constexpr float MAX_X =  1.6f;
inline constexpr float MIN_Y = -1.6f;
inline constexpr float MAX_Y =  3.0f;
inline constexpr float MIN_Z = -1.6f;
inline constexpr float MAX_Z =  1.6f;

}
