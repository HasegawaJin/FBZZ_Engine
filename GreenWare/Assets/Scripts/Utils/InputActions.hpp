/// @file InputActions.hpp
/// @brief ゲームが使う入力アクションの論理名。アタッチしないユーティリティ
/// @author Hasegawa Jin
/// @date 2026-08-23
///
/// WHY 名前を 1 箇所に集めるか:
///   アクション名は文字列で、綴りを間違えてもビルドは通り、実行時に
///   「そのボタンだけ無反応」という形でしか現れない。原因に辿り着くまでが長い。
///   定数にしておけば、綴り間違いはビルドエラーとして即座に出る。
///
/// WHY KeyCode を Inspector に持たせないのをやめたか:
///   スクリプトが物理入力を直接指すと、その 1 行のためにゲームパッド対応も
///   キーコンフィグも原理的に不可能になる (InputBinding.hpp の設計意図)。
///   スクリプトは論理名だけを知り、実際の割り当ては
///   ProjectSettings/Input.inputactions が持つ。
///
/// NOTE: ここの定数と Input.inputactions の name は 1 対 1 で対応する。
///       片方だけを直すと無反応になるため、必ず両方を同時に更新すること。
#pragma once

namespace sandbox::actions {

// ── 移動 (軸) ────────────────────────────────────────────────────────────────
// WASD と左スティックの両方が刺さる。input.GetMoveAxis() がこの 2 本を
// 半径デッドゾーン付きでまとめて返すため、通常は個別に読む必要はない。
inline constexpr const char* kMoveX = "MoveX";
inline constexpr const char* kMoveY = "MoveY";

// ── 極性エミッター (6 章) ────────────────────────────────────────────────────
// 11 章の操作表: 左入力 = 左銃 (−) / 右入力 = 右銃 (＋)。
inline constexpr const char* kEmitPlus  = "EmitPlus";
inline constexpr const char* kEmitMinus = "EmitMinus";

// ── 移動アクション ───────────────────────────────────────────────────────────
inline constexpr const char* kJump  = "Jump";
inline constexpr const char* kDodge = "Dodge";

// ── 銃の出し入れ ─────────────────────────────────────────────────────────────
// Toggle はパッド用。1 ボタンで往復させる代わりに、どちらへ動くかは
// 呼び出し側が今の状態から決める。
inline constexpr const char* kDrawWeapons    = "DrawWeapons";
inline constexpr const char* kHolsterWeapons = "HolsterWeapons";
inline constexpr const char* kToggleWeapons  = "ToggleWeapons";

// ── メニュー ─────────────────────────────────────────────────────────────────
inline constexpr const char* kPause  = "Pause";
inline constexpr const char* kSubmit = "Submit";
inline constexpr const char* kCancel = "Cancel";

/// OPTIONS 画面の CONTROLS に並ぶ 1 行。
///
/// WHY 行と割り当ての対応をここに置くか:
///   行 ID (Options.scene の CtrlDiv_<key> / CtrlR_<機器>_<key>) と論理名の対応は、
///   画面のレイアウトではなく「その行が何の操作か」という入力側の知識。UI 側に置くと、
///   行を足すたびに画面のコードとアクション表の両方を突き合わせることになる。
///
/// action が空の行は差し替えられない。軸 (移動・視点) は 1 行に 4 つのキーが乗って
/// いて「この行のキー」が 1 つに定まらず、起爆は照射と同じボタンのタップなので
/// 独立した割り当てを持たない。
struct ControlRow {
    const char* key;
    const char* action;
};

inline constexpr ControlRow kControlRows[] = {
    { "move",  "" },
    { "cam",   "" },
    { "lgun",  kEmitMinus },
    { "rgun",  kEmitPlus },
    { "fire",  "" },
    { "dodge", kDodge },
    { "pause", kPause },
};

} // namespace sandbox::actions
