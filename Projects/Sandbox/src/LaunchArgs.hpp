/// @file    LaunchArgs.hpp
/// @brief   Sandbox のコマンドライン引数解析。
/// @author  Hasegawa Jin
/// @date    2026-06-02
#pragma once

#include <filesystem>

namespace fbzz::sandbox {

/// --project と --standalone の解析結果。
/// Parse() は引数なし起動時の既定プロジェクト探索も行う。
struct LaunchArgs {
    std::filesystem::path projectPath;
    bool                  standalone = false;

    /// Win32 のコマンドラインを解析して起動モードを決定する。
    [[nodiscard]] static LaunchArgs Parse();
};

} // namespace fbzz::sandbox
