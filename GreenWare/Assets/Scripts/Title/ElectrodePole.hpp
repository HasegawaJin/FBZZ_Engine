/// @file    ElectrodePole.hpp
/// @brief   タイトル画面の電極演出だけが使う ± の極。遊びの側には存在しない。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// WHY タイトル専用に閉じるか:
/// 極性は遊びとしては撤去された。ただしタイトルの電極 (2 本の柱が引き合い、
/// 粒子と電界線が走る絵) は ± が «絵の仕組みそのもの» で、片方だけにすると成立しない。
/// 遊びが持たなくなった概念を Utils に残すと「まだ極性がある」と読まれるので、
/// 演出が使う ± は Title/ の中だけで完結させる。
/// 配色は BladeColors が唯一の正本 — ここで赤青を書き直さない。
#pragma once

#include <Scripts/Utils/BladeColors.hpp>

#include <Math/Vector4.hpp>

namespace sandbox {

// タイトルの電極が持つ極。演出専用で、盤面のどこにも出ない。
enum class Pole : int {
    Plus  = 0,  // 赤
    Minus = 1,  // 青
};

[[nodiscard]] inline fbzz::math::Vector4 PoleColor(Pole pole)
{
    return pole == Pole::Minus ? kColorLeft : kColorRight;
}

[[nodiscard]] inline Pole OppositePole(Pole pole)
{
    return pole == Pole::Plus ? Pole::Minus : Pole::Plus;
}

// 異極どうしか。引き合う唯一の条件。
[[nodiscard]] inline bool ArePolesAttracting(Pole a, Pole b) { return a != b; }

} // namespace sandbox
