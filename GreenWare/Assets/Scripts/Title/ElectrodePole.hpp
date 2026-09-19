/// @file    ElectrodePole.hpp
/// @brief   タイトル画面の電極演出だけが使う ± の極。遊びの側には存在しない。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// @note タイトル専用に閉じる理由: 極性は遊びとしては撤去されたが、タイトルの電極演出は
///       ± が «絵の仕組みそのもの» で片方だけにすると成立しない。Utils に残すと
///       「まだ極性がある」と読まれるため Title/ に閉じる。配色は BladeColors が正本。
#pragma once

#include <Scripts/Utils/BladeColors.hpp>

#include <Math/Vector4.hpp>

namespace sandbox {

/// タイトルの電極が持つ極。演出専用で、盤面のどこにも出ない。
enum class Pole : int {
    Plus  = 0,  ///< 赤
    Minus = 1,  ///< 青
};

[[nodiscard]] inline fbzz::math::Vector4 PoleColor(Pole pole)
{
    return pole == Pole::Minus ? kColorLeft : kColorRight;
}

[[nodiscard]] inline Pole OppositePole(Pole pole)
{
    return pole == Pole::Plus ? Pole::Minus : Pole::Plus;
}

/// 異極どうしか。引き合う唯一の条件。
[[nodiscard]] inline bool ArePolesAttracting(Pole a, Pole b) { return a != b; }

} // namespace sandbox
