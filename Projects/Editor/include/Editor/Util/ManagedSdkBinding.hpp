/// @file    ManagedSdkBinding.hpp
/// @brief   起動Editorに対応するSDK/repositoryとmanaged build/host/API配置を一緒に解決する。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#pragma once
#include <filesystem>
#include <string>
namespace fbzz::editor {
struct ManagedSdkBinding {
    std::filesystem::path root, agentBuild, hostDirectory, apiPath;
    std::string configuration;
    bool published = false;
};
/// @return 現在processの実行file。取得失敗は空。
std::filesystem::path ManagedEditorExecutable();
/// @return executableに整合する配置のみ成功。失敗はout未変更、diagnosticを返す。
/// @note published layoutはmanifest欠損でもsource repository探索へfallbackしない。source EditorはrepositoryのToolsを探索する。
bool ResolveManagedSdkBinding(const std::filesystem::path& executable, const std::filesystem::path& project,
    const std::filesystem::path& explicitRoot, const std::string& sourceConfiguration,
    ManagedSdkBinding& out, std::string& diagnostic);
/// @note environmentRootを優先し、空ならprojectの旧engine.sdk_root/rootを読む。相対指定はproject基準。
bool ResolveProjectManagedSdkBinding(const std::filesystem::path& executable, const std::filesystem::path& project,
    const std::filesystem::path& environmentRoot, const std::string& sourceConfiguration,
    ManagedSdkBinding& out, std::string& diagnostic);
} /// @note namespace fbzz::editor