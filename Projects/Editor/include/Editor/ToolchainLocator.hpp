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
        std::filesystem::path cmakeExe;
        std::filesystem::path buildDir;
        std::filesystem::path exeDebug;
        std::filesystem::path exeRelease;
        std::filesystem::path exeDevelopment;
        /// @note スクリプト DLL のパス (build.config の scripts_dll_* から解決)。DLL 名はプロジェクトごとに異なる (SandboxScripts / MyGameScripts 等) ため build.config に記録し動的に解決する。
        std::filesystem::path scriptsDllDebug;
        std::filesystem::path scriptsDllRelease;
        std::filesystem::path scriptsDllDevelopment;
        bool                  found = false;
        std::string           error;
    };

    /// buildRoot/build.config を優先し、空ならエディタ exe から親方向に build.config を探索する。
    [[nodiscard]] static Result Locate(const std::filesystem::path& buildRoot = {});
};

} // namespace fbzz::editor
