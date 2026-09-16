/// @file    InputActions.hpp
/// @brief   ゲームが使う入力アクションの論理名。アタッチしないユーティリティ
/// @author  Hasegawa Jin
/// @date    2026-08-23
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

// ── 斬撃 (Docs/blades.md) ────────────────────────────────────────────────────
// 斬るのはボタン 1 つ。どちらの刀が出るかは連撃の段が決める
// (BladeComponent::NextSwingSide)。
//
// WHY 左右 2 ボタンをやめたか (2026-09-08): 押し分ける理由は «乗せる極を選ぶ» こと
//     だったが、極性は休眠していて左右は絵の違いだけになっている。意味の無い選択に
//     2 ボタン使うより、空いた右クリックを弾きへ回す方が芯に近い指の形になる。
//
// WHY 綴りが "EmitPlus" のままか: Input.inputactions の name と 1 対 1 で、さらに
//     settings.toml の [bind] がこの綴りでユーザーの差し替えを保存している。改名すると
//     既存のキーコンフィグが黙って «既定に戻った» ように見える。照射だった頃の名残だが
//     画面に出る文字ではないので、綴りは据え置いて呼び名だけ実態へ合わせる。
inline constexpr const char* kAttack = "EmitPlus";

// ── 移動アクション ───────────────────────────────────────────────────────────
inline constexpr const char* kJump  = "Jump";
inline constexpr const char* kDodge = "Dodge";
// 弾き。ボスが倒れている間は同じボタンが «とどめ» になる (Docs/break-parry.md)。
inline constexpr const char* kParry = "Parry";
// 転倒したボスの脚に取り付いて登る (Docs/climb-core.md)。
//
// WHY ジャンプと分けるか: 取り付ける場所 (倒れた脚のそば) は跳びたい場所でもある。
//     兼ねさせると «跳ぼうとして登る» が必ず起きるが、登りは 3 秒以上を拘束するので
//     取り違えた側の代償が大きい。
inline constexpr const char* kClimb = "Climb";

// ── 刀の出し入れ ─────────────────────────────────────────────────────────────
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
/// いて「この行のキー」が 1 つに定まらないため。
///
/// NOTE: 行を足す / key を変えるときは Options.scene の
///       `CtrlDiv_<key>` `CtrlL_<key>` `CtrlR_<機器>_<key>` `CtrlIcon_<機器>_<key>_<n>`
///       も同じ綴りへ揃えること。見つからない行は警告だけ出して黙って消える。
struct ControlRow {
    const char* key;
    const char* action;
};

inline constexpr ControlRow kControlRows[] = {
    { "move",   "" },
    { "cam",    "" },
    { "blade",  kAttack },
    { "parry",  kParry },
    { "dodge",  kDodge },
    { "jump",   kJump },
    { "climb",  kClimb },
    { "pause",  kPause },
};

} // namespace sandbox::actions
