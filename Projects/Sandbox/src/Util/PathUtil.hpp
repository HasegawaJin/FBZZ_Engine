// FBZZ Engine
// PathUtil.hpp | fbzz::sandbox::util
// Sandbox 起動処理で使う Windows / UTF-8 / filesystem 変換ユーティリティ
#pragma once

#include <filesystem>
#include <string>

namespace fbzz::sandbox::util {

/// UTF-8 文字列を Windows API 用の wide string に変換する。
/// WHY: ProjectSettings は UTF-8、Win32 API は wide string を主に使うため境界を明示する。
[[nodiscard]] std::wstring Utf8ToWide(const std::string& text);

/// Windows API 由来の wide string をエンジン内で扱う UTF-8 文字列へ変換する。
[[nodiscard]] std::string WideToUtf8(const std::wstring& text);

/// filesystem::path を UTF-8 文字列へ変換する。
[[nodiscard]] std::string PathToUtf8(const std::filesystem::path& path);

/// 相対パスを絶対パスへ正規化する。失敗時は入力をそのまま返す。
[[nodiscard]] std::filesystem::path MakeAbsolute(const std::filesystem::path& path);

/// 実行ファイルが存在するディレクトリを返す。
[[nodiscard]] std::filesystem::path GetExecutableDirectory();

} // namespace fbzz::sandbox::util
