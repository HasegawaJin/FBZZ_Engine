// FBZZ Engine
// LaunchArgs.hpp | fbzz::sandbox
// Sandbox のコマンドライン引数解析
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
