/// @file    ToolchainLocator.hpp
/// @brief   RuntimeBuild が使用する CMake とビルド成果物パスの解決。
/// @author  Hasegawa Jin
/// @date    2026-06-02
#pragma once

#include <filesystem>
#include <string>

namespace fbzz::editor {

/// RuntimeBuild に必要な cmake.exe、CMake キャッシュ、standalone exe の場所を解決する。
/// @note ユーザーにターミナル上の CMake パスを入力させないため、configure 時生成の build.config を優先して VS のマルチ構成出力へ追従する。
class ToolchainLocator {
public:
    struct Result {
        std::filesystem::path cmakeExe; ///< 配布ビルドを実行する cmake.exe の解決済みパス。
        std::filesystem::path buildDir; ///< CMake のキャッシュと構成済みビルドツリーの場所。
        std::filesystem::path exeDebug; ///< Debug 構成が出力する Standalone の実行ファイルパス。
        std::filesystem::path exeRelease; ///< Release 構成が出力する Standalone の実行ファイルパス。
        std::filesystem::path exeDevelopment; ///< Development 構成が出力する Standalone の実行ファイルパス。
        bool                  found = false; ///< CMake とビルドツリーの両方を解決できたか。
        std::string           error; ///< ビルド環境を解決できなかった理由。
    };

    /// buildRoot/build.config を優先し、空ならエディタ exe から親方向に build.config を探索する。
    [[nodiscard]] static Result Locate(const std::filesystem::path& buildRoot = {});
};

} // namespace fbzz::editor
