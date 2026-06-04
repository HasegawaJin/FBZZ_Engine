// FBZZ Engine
// HubUtil.hpp | fbzz::hub::util
// GameHub 内で共有する Win32 / UTF-8 / filesystem 変換ユーティリティ
//
// WHY: GameHub は fbzz_engine に非依存のスタンドアロン Win32 アプリであるため
//      Engine/Util/StringUtils を使えない。各 .cpp に散在していた同一実装を
//      一箇所に集約して重複を排除する。
#pragma once

#include <filesystem>
#include <string>

namespace fbzz::hub::util {

/// UTF-8 文字列を Windows API 用の wide string に変換する。
[[nodiscard]] std::wstring Utf8ToWide(const std::string& text);

/// Windows API 由来の wide string を UTF-8 文字列へ変換する。
[[nodiscard]] std::string WideToUtf8(const std::wstring& text);

/// 実行ファイルが存在するディレクトリを返す。
[[nodiscard]] std::filesystem::path GetExecutableDirectory();

} // namespace fbzz::hub::util
