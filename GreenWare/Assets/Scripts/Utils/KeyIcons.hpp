/// @file    KeyIcons.hpp
/// @brief   入力アイコン (Sprites/HUD) の Sprite 参照をまとめた表
/// @author  Hasegawa Jin
/// @date    2026-08-24
///
/// ⚠ このファイルは Tools の生成物。手で編集しないこと。
///   セル番号 → 意味の対応は目視で決めたもので、生成器 (gen_keyicons.js) が持っている。
///
/// WHY 生成するか:
///   シートのスプライト名は _0 .. _271 の連番だけで、どの番号がどのキーかは
///   .meta にもどこにも書かれていない。番号から GUID を人が引き直すと必ず写し間違える。
///
/// WHY Sprite 参照を文字列で持つか:
///   UIImage.texturePath / ui.SetImageTexture() が "<png>::sprite::<id>" を直接受ける。
///   切り出し矩形とピボットは .meta から自動で解決されるので、呼ぶ側は寸法を知らなくてよい。
///
/// ⚠ 表そのもの (kbm:: / pad:: の定数) は Tools の生成物。手で編集しないこと。
///   下の Action / Prompt は «どの操作にどの絵を当てるか» という遊び側の決定なので、
///   ここは手で書く。
///
/// 使い方:
///   ui.SetImageTexture(go, keyicon::pad::kA);
///   ui.SetImageTexture(go, keyicon::Prompt(usingPad, keyicon::Action::Dodge));
#pragma once

#include <string_view>

namespace sandbox::keyicon {

/// keyboard-&-mouse_sheet_default.png (272 個) から必要なぶんだけ。
namespace kbm {
inline constexpr const char* kW =
    "Assets/Sprites/HUD/keyboard-&-mouse_sheet_default.png::sprite::836d2b4d-ed00-4420-ba95-fc329c7005e5";   // cell 36
inline constexpr const char* kA =
    "Assets/Sprites/HUD/keyboard-&-mouse_sheet_default.png::sprite::71214115-6220-48b8-a69b-bec26806a7ba";   // cell 242
inline constexpr const char* kS =
    "Assets/Sprites/HUD/keyboard-&-mouse_sheet_default.png::sprite::65f30850-e85b-4263-8d8a-0ac299714038";   // cell 70
inline constexpr const char* kD =
    "Assets/Sprites/HUD/keyboard-&-mouse_sheet_default.png::sprite::777b3b57-9618-4a85-a26b-1120a12408cc";   // cell 171
inline constexpr const char* kSpace =
    "Assets/Sprites/HUD/keyboard-&-mouse_sheet_default.png::sprite::285b99f9-693b-425d-af31-9e341253b3a7";   // cell 53
inline constexpr const char* kEsc =
    "Assets/Sprites/HUD/keyboard-&-mouse_sheet_default.png::sprite::b65d1571-1356-4f11-80b4-ac4b6559fe39";   // cell 183
inline constexpr const char* kMouse =
    "Assets/Sprites/HUD/keyboard-&-mouse_sheet_default.png::sprite::d7bd3d66-0ca3-4bb6-a65c-ec6d975fbc98";   // cell 17
inline constexpr const char* kMouseLeft =
    "Assets/Sprites/HUD/keyboard-&-mouse_sheet_default.png::sprite::20377992-6a61-48a2-901a-1aaf1e4cd1d9";   // cell 49
inline constexpr const char* kMouseRight =
    "Assets/Sprites/HUD/keyboard-&-mouse_sheet_default.png::sprite::e0b68501-d61b-4aa2-a159-6de4978a23eb";   // cell 19
inline constexpr const char* kShift =
    "Assets/Sprites/HUD/keyboard-&-mouse_sheet_default.png::sprite::0f64bf66-5622-4cd0-aa8b-c16200307c7e";   // cell 76
inline constexpr const char* kCtrl =
    "Assets/Sprites/HUD/keyboard-&-mouse_sheet_default.png::sprite::ae37b767-c800-4c16-a24b-daaa73664131";   // cell 170
inline constexpr const char* kEnter =
    "Assets/Sprites/HUD/keyboard-&-mouse_sheet_default.png::sprite::38dc1936-2db2-4725-bb85-7c667e6da72b";   // cell 103
inline constexpr const char* kTab =
    "Assets/Sprites/HUD/keyboard-&-mouse_sheet_default.png::sprite::32517aa3-6101-4ea4-83df-9af9dd6bac71";   // cell 56
inline constexpr const char* kE =
    "Assets/Sprites/HUD/keyboard-&-mouse_sheet_default.png::sprite::b887f40c-d4b1-4bb5-88f7-22e0865d8b91";   // cell 175
inline constexpr const char* kQ =
    "Assets/Sprites/HUD/keyboard-&-mouse_sheet_default.png::sprite::2d8a69a1-674f-4aa5-ab71-df15c7bfb8fa";   // cell 94
inline constexpr const char* kR =
    "Assets/Sprites/HUD/keyboard-&-mouse_sheet_default.png::sprite::d6619bfc-9c95-4fa9-b57a-a662d40bf08c";   // cell 100
inline constexpr const char* kF =
    "Assets/Sprites/HUD/keyboard-&-mouse_sheet_default.png::sprite::a5893159-424e-4355-bbde-c7df069002c3";   // cell 153
} // namespace kbm

/// xbox-series_sheet_default.png (100 個) から必要なぶんだけ。
namespace pad {
inline constexpr const char* kA =
    "Assets/Sprites/HUD/xbox-series_sheet_default.png::sprite::3cb1a753-0e0e-45e8-b278-55f88fdf36dd";   // cell 82
inline constexpr const char* kB =
    "Assets/Sprites/HUD/xbox-series_sheet_default.png::sprite::cf52d251-9771-41b7-bded-c444768c0468";   // cell 84
inline constexpr const char* kX =
    "Assets/Sprites/HUD/xbox-series_sheet_default.png::sprite::2d971312-1470-4286-a32b-82fb8df379d7";   // cell 86
inline constexpr const char* kY =
    "Assets/Sprites/HUD/xbox-series_sheet_default.png::sprite::1d641538-8125-4626-866d-73c96a1fa77b";   // cell 88
inline constexpr const char* kLB =
    "Assets/Sprites/HUD/xbox-series_sheet_default.png::sprite::5b07f278-b88f-4abc-8b97-eda5c05ce19b";   // cell 37
inline constexpr const char* kRB =
    "Assets/Sprites/HUD/xbox-series_sheet_default.png::sprite::a012fae6-e0fd-4352-bd65-c142066822a7";   // cell 23
inline constexpr const char* kLT =
    "Assets/Sprites/HUD/xbox-series_sheet_default.png::sprite::ad36dbb2-d2e1-4f98-b852-90c89d0ef870";   // cell 21
inline constexpr const char* kRT =
    "Assets/Sprites/HUD/xbox-series_sheet_default.png::sprite::2f701cf0-6ff5-44a4-9d03-5420e3e16f4f";   // cell 27
inline constexpr const char* kLS =
    "Assets/Sprites/HUD/xbox-series_sheet_default.png::sprite::3b47d37d-2b6b-4441-a362-d2d3a489ff5e";   // cell 20
inline constexpr const char* kRS =
    "Assets/Sprites/HUD/xbox-series_sheet_default.png::sprite::8753a010-85f6-4826-8463-16f3b34d080a";   // cell 25
inline constexpr const char* kStart =
    "Assets/Sprites/HUD/xbox-series_sheet_default.png::sprite::24e7e554-9294-46ab-82e5-637031a36612";   // cell 74
inline constexpr const char* kBack =
    "Assets/Sprites/HUD/xbox-series_sheet_default.png::sprite::02871d20-cab3-42ef-9648-37cbdd518f92";   // cell 81
inline constexpr const char* kDPad =
    "Assets/Sprites/HUD/xbox-series_sheet_default.png::sprite::d21d68ca-aa0a-4418-8cd9-16c8e5232609";   // cell 63
} // namespace pad

/// ゲーム中のプロンプトで使う操作の種類。
enum class Action {
    MoveAxis, LookAxis, Attack, Parry, Dodge, Jump, Pause, Confirm, Cancel, Climb,
};

/// 入力機器に応じたアイコンを返す。該当が無ければ空文字列 (呼び出し側は文字で出す)。
///
/// WHY 1 つの関数にまとめるか: プロンプトを出す側は「回避のボタン」を知りたいだけで、
///     パッドかキーボードかで分岐を書きたいわけではない。分岐をここに閉じる。
///
/// ⚠ 返す絵は ProjectSettings/Input.inputactions の «既定の» 割り当てと対にすること。
///   差し替えた行はアイコンをやめて文字を出す仕組みなので (OptionsScreenComponent)、
///   ここがずれると «既定のまま遊んでいる人にだけ違うキーが出る» という形で出る。
[[nodiscard]] inline std::string_view Prompt(bool usingPad, Action action)
{
    switch (action) {
    case Action::MoveAxis: return usingPad ? pad::kLS    : kbm::kW;      // 代表 1 つ
    case Action::LookAxis: return usingPad ? pad::kRS    : kbm::kMouse;
    case Action::Attack:   return usingPad ? pad::kRT    : kbm::kMouseLeft;
    case Action::Parry:    return usingPad ? pad::kLB    : kbm::kMouseRight;
    case Action::Dodge:    return usingPad ? pad::kB     : kbm::kShift;
    case Action::Jump:     return usingPad ? pad::kA     : kbm::kSpace;
    case Action::Pause:    return usingPad ? pad::kStart : kbm::kEsc;
    case Action::Confirm:  return usingPad ? pad::kA     : kbm::kEnter;
    case Action::Cancel:   return usingPad ? pad::kB     : kbm::kEsc;
    // 登る (Input.inputactions の Climb)。E は WASD の隣で、走り寄って押すまでが
    // 指 1 本で繋がる。パッドは Y ─ 攻撃 (RT) や回避 (B) と指が競合しない。
    case Action::Climb:    return usingPad ? pad::kY     : kbm::kE;
    }
    return {};
}

} // namespace sandbox::keyicon
