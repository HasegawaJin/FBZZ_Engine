// FBZZ Engine
// FileUtil.hpp | fbzz::sandbox::util
// Sandbox のプロジェクト解決で使うファイル読み取りユーティリティ
#pragma once

#include <filesystem>
#include <string>

namespace fbzz::sandbox::util {

/// ファイルまたはディレクトリの存在を例外なしで確認する。
[[nodiscard]] bool Exists(const std::filesystem::path& path);

/// テキストファイル全体を読み込む。失敗時は空文字列を返す。
[[nodiscard]] std::string ReadText(const std::filesystem::path& path);

} // namespace fbzz::sandbox::util
