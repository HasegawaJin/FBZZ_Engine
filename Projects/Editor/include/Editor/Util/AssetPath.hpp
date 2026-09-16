/// @file    AssetPath.hpp
/// @brief   Editor 内で共有する Assets 起点パスの正規化ユーティリティ。
/// @author  Hasegawa Jin
/// @date    2026-06-07
#pragma once

#include <string>
#include <string_view>

namespace fbzz::editor {

// NormalizeAssetPath — OS 絶対パス / バックスラッシュ混在パスを Assets 起点の保存用パスへ寄せる。
// WHY: AssetBrowser は絶対パスを持ち、Scene / Component / .mat は Assets 起点パスを保存する。
//      ここを各パネルで個別実装すると、ロック・DragDrop・保存時に別形式のパスが混ざりやすい。
[[nodiscard]] std::string NormalizeAssetPath(std::string path);

// ToProjectAssetDiskPath — Assets 起点パスをプロジェクトルートからの実ファイルパスへ変換する。
// WHY: Runtime は可搬な Assets 起点パスだけを保持し、Editor の保存 / 読み込み時だけ projectRoot を補完する。
[[nodiscard]] std::string ToProjectAssetDiskPath(std::string_view projectRoot, std::string_view assetPath);

} // namespace fbzz::editor
