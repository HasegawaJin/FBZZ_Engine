/// @file    InputActions.hpp
/// @brief   ゲームが使う入力アクションの論理名。アタッチしないユーティリティ
/// @author  Hasegawa Jin
/// @date    2026-08-23
///
/// @note 定数にするのは、綴り間違いを実行時の「そのボタンだけ無反応」からビルドエラーへ
///       変えるため。スクリプトは論理名だけを知り、物理入力の割り当ては
///       ProjectSettings/Input.inputactions が持つ (パッド対応・キーコンフィグを可能にする)。
///       ここの定数と Input.inputactions の name は 1 対 1 対応。片方だけ直すと無反応になる。
#pragma once

namespace sandbox::actions {

/// WASD と左スティックの両方が刺さる。input.GetMoveAxis() がこの 2 本を
/// 半径デッドゾーン付きでまとめて返すため、通常は個別に読む必要はない。
inline constexpr const char* kMoveX = "MoveX";
inline constexpr const char* kMoveY = "MoveY";

/// 斬るのはボタン 1 つ。どちらの刀が出るかは連撃の段が決める (BladeComponent::NextSwingSide)。
/// @note 左右 2 ボタンをやめた (2026-09-08)。押し分けは «乗せる極を選ぶ» ためだったが、極性は
///       休眠し左右は絵の違いだけになったため、空いた右クリックを弾きへ回した。
/// @note 綴りが "EmitPlus" のままなのは Input.inputactions の name および settings.toml の
///       [bind] のユーザー差し替え保存と 1 対 1 のため。改名すると既存のキーコンフィグが
///       黙って既定に戻る。照射だった頃の名残だが画面に出ない文字なので据え置く。
inline constexpr const char* kAttack = "EmitPlus";

inline constexpr const char* kJump  = "Jump";
inline constexpr const char* kDodge = "Dodge";
/// 弾き。ボスが倒れている間は同じボタンが «とどめ» になる (Docs/break-parry.md)。
inline constexpr const char* kParry = "Parry";
/// 転倒したボスの脚に取り付いて登る (Docs/climb-core.md)。
/// @note ジャンプと分けるのは、取り付く場所 (倒れた脚のそば) が跳びたい場所でもあり兼ねると
///       «跳ぼうとして登る» が必ず起きるため。登りは 3 秒以上拘束するので取り違えの代償が大きい。
inline constexpr const char* kClimb = "Climb";

/// Toggle はパッド用。1 ボタンで往復させる代わりに、どちらへ動くかは
/// 呼び出し側が今の状態から決める。
inline constexpr const char* kDrawWeapons    = "DrawWeapons";
inline constexpr const char* kHolsterWeapons = "HolsterWeapons";
inline constexpr const char* kToggleWeapons  = "ToggleWeapons";

inline constexpr const char* kPause  = "Pause";
inline constexpr const char* kSubmit = "Submit";
inline constexpr const char* kCancel = "Cancel";

/// OPTIONS 画面の CONTROLS に並ぶ 1 行。
/// @note 行 ID (Options.scene の `CtrlDiv_<key>` / `CtrlR_<機器>_<key>`) と論理名の対応を
///       ここに置くのは、画面レイアウトでなく「その行が何の操作か」という入力側の知識の
///       ため。action が空の行 (軸: 移動・視点) は 1 行に複数キーが乗り 1 つに定まらない
///       ため差し替えられない。行を足す/key を変えるときは Options.scene の `CtrlDiv_<key>`
///       `CtrlL_<key>` `CtrlR_<機器>_<key>` `CtrlIcon_<機器>_<key>_<n>` も揃えること
///       (見つからない行は警告だけ出して黙って消える)。
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
