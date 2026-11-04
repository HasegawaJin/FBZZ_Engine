/// @file    AtomicFileReplace.hpp
/// @brief   書き込み済み一時ファイルを公開する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#pragma once
#include <Core/Api.hpp>
#include <filesystem>
#include <system_error>

namespace fbzz::util {

/// @brief 同一ボリュームの一時ファイルを既存の対象と原子的に置き換える。
/// @note 削除共有で開いた読み手は古いファイルをそのまま読める。失敗時は一時ファイルを保持する。
[[nodiscard]] FBZZ_CORE_API std::error_code ReplaceFileAtomic(const std::filesystem::path& temporary,
                                                               const std::filesystem::path& target);

} /// @note namespace fbzz::util
