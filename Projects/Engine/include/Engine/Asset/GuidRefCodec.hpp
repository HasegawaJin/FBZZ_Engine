/// @file    GuidRefCodec.hpp
/// @brief   TOML シリアライズ境界でのアセット参照 ⇄ GUID 変換。
/// @author  Hasegawa Jin
/// @date    2026-07-08
///
/// 設計: ランタイム (コンポーネント / Inspector) は常に "Assets/..." パス文字列を持ち、
/// ディスク上 (.scene / .mat / .terrain / .animcontroller) だけが "guid:<32hex>" を持つ。
/// Save 直前に Encode、Load 直後に Decode することで、既存のコンポーネント定義や
/// エディター UI を一切変えずにリネーム・移動耐性を得る。
/// @note 参照フィールドの型は string のまま。AssetManager::ResolvePath にも guid: 分岐が
///       あるため、デコード漏れがあってもロードは壊れない (二重の安全網)。
///
/// 変換規則:
/// Encode: AssetDatabase で guid に解決できた文字列値のみ "guid:..." へ置換。
/// baked (.fzasset/.mesh 等 guid を持たない) や未登録パスは素通り。
/// Decode: "guid:..." のみ "Assets/..." 相対パスへ置換。解決不能なら元のまま。
///
/// ディスク上の形式は "guid:<32hex>[<サブアセット接尾辞>]|<プロジェクト相対パス>"。
/// '|' 以降は人と外部ツールのためのヒントで、権威は常に guid 側にある。
/// @note guid だけだと「どのファイルか」をファイルの中から辿れない。特に Library/Baked の
///       導出 guid は DeriveGuid の計算結果でしかなく、ディスク上のどこにも文字列として
///       存在しないため grep でも見つからない。副次的に、.meta を作り直して guid が変わった
///       場合の復旧経路にもなる (guid が引けずヒントの実体が残っていればそちらを使う)。
#pragma once
#include <toml++/toml.hpp>
#include <string>

namespace fbzz::asset {

/// 単一文字列の変換。参照でない文字列はそのまま返る (冪等)。
[[nodiscard]] std::string EncodeGuidRef(const std::string& pathOrRef);
[[nodiscard]] std::string DecodeGuidRef(const std::string& ref);

/// TOML ツリー内の全文字列値へ再帰適用する (table / array 対応)。
void EncodeGuidRefs(toml::table& root);
void DecodeGuidRefs(toml::table& root);

} // namespace fbzz::asset
