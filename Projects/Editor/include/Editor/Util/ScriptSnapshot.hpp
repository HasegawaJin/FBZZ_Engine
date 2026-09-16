/// @file    ScriptSnapshot.hpp
/// @brief   Script 1 個ぶんの Reflect() 可能なフィールドを TOML 文字列へ出し入れする。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// WHY: Inspector でスクリプトのフィールドを触ったときの Undo は、これまで
/// 「シーン全体を TOML 化して before/after にする」方式しか無かった。
/// スクリプトの実体は DLL の向こう側にあり、型を知らないエディタからは
/// 値を取り出せない、というのが理由だった。
///
/// だが Script は Reflect() を実装している。IReflector を 1 つ書けば、
/// 型を知らないまま「そのスクリプトだけ」を読み書きできる。
/// これで Undo が全シーン再構築ではなくスクリプト 1 個の値復元になり、
/// EntityID も選択も維持される (シーンが作り直されない)。
#pragma once
#include <string>

namespace fbzz::scene { class Script; }

namespace fbzz::editor {

// script の全 Reflect フィールドを TOML テキストとして書き出す。
[[nodiscard]] std::string CaptureScriptSnapshot(scene::Script& script);

// CaptureScriptSnapshot の出力を script へ書き戻す。
// スナップショットに無いフィールドは現在値のまま残る (フィールド追加に耐える)。
bool ApplyScriptSnapshot(scene::Script& script, const std::string& snapshot);

} // namespace fbzz::editor
