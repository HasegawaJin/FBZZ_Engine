/// @file    AssetPath.hpp
/// @brief   Editor 内で共有する Assets 起点パスの正規化ユーティリティ。
/// @author  Hasegawa Jin
/// @date    2026-06-07
#pragma once

#include <string>
#include <string_view>

namespace fbzz::editor {

/// @brief OS 絶対パス / バックスラッシュ混在パスを Assets 起点の保存用パスへ寄せる。
/// @note AssetBrowser は絶対パスを持ち、Scene / Component / .mat は Assets 起点パスを保存する。
/// @note GUID 参照はヒントを含めそのまま保持する。
[[nodiscard]] std::string NormalizeAssetPath(std::string path);

/// @brief Assets 起点パスまたは GUID 参照を実ファイルパスへ変換する。
/// @note GUID は現在の索引から解決する。未登録なら空文字列で、古いヒント先へ保存しない。
[[nodiscard]] std::string ToProjectAssetDiskPath(std::string_view projectRoot, std::string_view assetPath);

} /// @note namespace fbzz::editor
