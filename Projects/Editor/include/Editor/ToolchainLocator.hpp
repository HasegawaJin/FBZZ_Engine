// FBZZ Engine
// ToolchainLocator.hpp | fbzz::editor
// RuntimeBuild が使用する CMake とビルド成果物パスの解決
#pragma once

#include <filesystem>
#include <string>

namespace fbzz::editor {

/// RuntimeBuild に必要な cmake.exe、CMake キャッシュ、standalone exe の場所を解決する。
/// WHY: エディタ UI からビルドするため、ユーザーにターミナル上の CMake パスを入力させず、
///      configure 時に生成された build.config を優先して Visual Studio のマルチ構成出力へ追従する。
class ToolchainLocator {
public:
    struct Result {
        std::filesystem::path cmakeExe;
        std::filesystem::path buildDir;
        std::filesystem::path exeDebug;
        std::filesystem::path exeRelease;
        bool                  found = false;
        std::string           error;
    };

    /// buildRoot/build.config を優先し、空ならエディタ exe から親方向に build.config を探索する。
    [[nodiscard]] static Result Locate(const std::filesystem::path& buildRoot = {});
};

} // namespace fbzz::editor
