/// @file    LaunchArgs.hpp
/// @brief   Sandbox のコマンドライン引数解析。
/// @author  Hasegawa Jin
/// @date    2026-06-02
#pragma once

#include <filesystem>

namespace fbzz::sandbox {

/// @brief コマンドラインの指定と実行ファイルの配置から決めた起動先。
struct LaunchArgs {
    /// @note 明示指定がなければ実行ファイルと同じ場所か既定プロジェクトを探し、見つからなければ空になる。
    std::filesystem::path projectPath;
    /// @note 配布用の配置または明示指定のときに、編集画面を開かずゲームを起動する。
    bool                  standalone = false;

    /// @return --project を優先し、省略時は実行ファイルの配置から補った起動設定。
    [[nodiscard]] static LaunchArgs Parse();
};

} /// @note namespace fbzz::sandbox
