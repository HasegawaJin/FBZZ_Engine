/// @file    ScriptSnapshot.hpp
/// @brief   Script 1 個ぶんの Reflect() 可能なフィールドを TOML 文字列へ出し入れする。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// @note スクリプトの実体は DLL の向こうにあり型を知らないエディタからは値を取り出せないため、
///       Undo は「シーン全体を TOML 化して before/after」方式しか無かった。Script は Reflect() を
///       実装するため IReflector 経由で 1 個だけ読み書きでき、EntityID も選択も維持される。
#pragma once
#include <string>

namespace fbzz::scene { class Script; }

namespace fbzz::editor {

/// script の全 Reflect フィールドを TOML テキストとして書き出す。
[[nodiscard]] std::string CaptureScriptSnapshot(scene::Script& script);

/// CaptureScriptSnapshot の出力を script へ書き戻す。
/// スナップショットに無いフィールドは現在値のまま残る (フィールド追加に耐える)。
bool ApplyScriptSnapshot(scene::Script& script, const std::string& snapshot);

} // namespace fbzz::editor
