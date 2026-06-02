// FBZZ Engine
// LaunchProject.hpp | fbzz::sandbox
// Sandbox 起動時に解決済みプロジェクトパスを保持する値型
#pragma once

#include <filesystem>

namespace fbzz::sandbox {

/// .fbzz_proj / ProjectSettings / 開始シーンの解決結果。
/// WHY: Module はファイル探索を行わず、解決済みパスだけを受け取って実行責務に集中する。
struct LaunchProject {
    std::filesystem::path root;
    std::filesystem::path projectFile;
    std::filesystem::path settingsFile;
    std::filesystem::path sceneFile;
};

} // namespace fbzz::sandbox
