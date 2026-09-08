/// @file    BladeColors.hpp
/// @brief   二刀の左右と、盤面共通の配色。アタッチしないユーティリティ (FBZZ_SCRIPT を持たない)。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// WHY 1 ファイルに集めるか:
/// 発光色に意味を持たせている以上、色が混ざると盤面が読めなくなる。
/// 右刀 = 赤 / 左刀 = 青 / プレイヤー = 緑 という対応が刀・HUD・VFX に散ると、
/// どれか 1 つを直したときに必ずズレる。ズレた瞬間にゲームが読めなくなる種類の値なので、
/// 定義を 1 箇所に閉じ込める。
///
/// WHY 極性 (BladeSide) ではなく左右 (BladeSide) か:
/// 赤青はもともと ＋/− の極を表していたが、極性の遊びは撤去された。
/// 今この 2 色が区別しているのは「どちらの刀が出たか」だけで、盤面の状態ではない。
/// 型の名前を遊びの実体へ合わせておかないと、次に読む人が「極がまだ在る」と誤読する。
#pragma once

#include <Math/MathUtils.hpp>
#include <Math/Vector4.hpp>

namespace sandbox {

// どちらの刀か。None = どちらでもない (振っていない / 色を持たない出来事)。
enum class BladeSide : int {
    None  = 0,
    Right = 1,  // 右刀 / 赤
    Left  = 2,  // 左刀 / 青
};

// ── 配色 ────────────────────────────────────────────────────────────────────
//
// WHY 彩度を上げて強度を抑えるか:
//   発光を強くしすぎると白飛びして赤と青の区別がつかなくなる。色そのものは飽和に
//   近づけ、明るさは kEmissiveBase で別に持って抑えめに出す。
//   「遠くからでも赤青が読めること」が明るさより優先される。
inline constexpr fbzz::math::Vector4 kColorRight  { 1.00f, 0.12f, 0.14f, 1.0f }; // 右刀 赤
inline constexpr fbzz::math::Vector4 kColorLeft   { 0.10f, 0.35f, 1.00f, 1.0f }; // 左刀 青
inline constexpr fbzz::math::Vector4 kColorPlayer { 0.20f, 1.00f, 0.45f, 1.0f }; // プレイヤー 緑
// 色を持たない状態は無彩色の金属。
inline constexpr fbzz::math::Vector4 kColorNeutral{ 0.55f, 0.57f, 0.60f, 1.0f };
// 警告色。赤と青のどちらとも取り違えない色でなければ、警告そのものが刀の色に見える。
// 赤青が塞いでいない残りの領域は黄なので、そこへ置く。
inline constexpr fbzz::math::Vector4 kColorWarning{ 1.00f, 0.82f, 0.10f, 1.0f };

// 発光の基準強度。白飛びさせないため 1.0 より低く始める。
inline constexpr float kEmissiveBase = 0.65f;

[[nodiscard]] inline fbzz::math::Vector4 BladeColor(BladeSide side)
{
    switch (side) {
    case BladeSide::Right: return kColorRight;
    case BladeSide::Left:  return kColorLeft;
    case BladeSide::None:  break;
    }
    return kColorNeutral;
}

// UI に出す記号。色覚に頼らず読めるようにするための二重表現。
[[nodiscard]] inline const char* BladeSymbol(BladeSide side)
{
    switch (side) {
    case BladeSide::Right: return "R";
    case BladeSide::Left:  return "L";
    case BladeSide::None:  break;
    }
    return "";
}

[[nodiscard]] inline BladeSide OppositeSide(BladeSide side)
{
    switch (side) {
    case BladeSide::Right: return BladeSide::Left;
    case BladeSide::Left:  return BladeSide::Right;
    case BladeSide::None:  break;
    }
    return BladeSide::None;
}

} // namespace sandbox
