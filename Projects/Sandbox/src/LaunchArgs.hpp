/// @file    LaunchArgs.hpp
/// @brief   Sandbox のコマンドライン引数解析。
/// @author  Hasegawa Jin
/// @date    2026-06-02
#pragma once

#include <filesystem>

namespace fbzz::sandbox {

/// @brief コマンドラインの指定と実行ファイルの配置から決めた起動先。
struct LaunchArgs {
    std::filesystem::path projectPath; ///< 開くプロジェクトのパス。指定も探索結果もない場合は空。
    bool standalone = false; ///< 明示指定または配布用の配置で、ゲームを起動する場合に true。

    /// @return --project を優先し、省略時は実行ファイルの配置から補った起動設定。
    [[nodiscard]] static LaunchArgs Parse();
};

} // namespace fbzz::sandbox
