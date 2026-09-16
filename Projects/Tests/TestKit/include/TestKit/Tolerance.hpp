/// @file    Tolerance.hpp
/// @brief   浮動小数点比較の許容誤差。テストにマジックナンバーを書かせないための定数。
/// @author  Hasegawa Jin
/// @date    2026-09-02
#pragma once

namespace fbzz::testkit {

/// 単発の演算 (正規化・内積・行列積) 向け。
/// float の仮数部は 24bit なので、1.0 付近の 1 回の演算誤差は 1e-7 前後に収まる。
/// 数回の積和を経ても届かない桁として 1e-5 を採る。
inline constexpr float kTolerance = 1.0e-5f;

/// 反復・積分を経た結果 (XPBD の収束位置、Slerp の連鎖、IK の追従) 向け。
/// 誤差が刻み数に比例して積み上がるため、単発と同じ桁では判定できない。
inline constexpr float kLooseTolerance = 1.0e-3f;

/// 「ほぼ 0」の判定。長さ・行列式・内積が退化しているかを見るときに使う。
inline constexpr float kEpsilon = 1.0e-6f;

} // namespace fbzz::testkit
