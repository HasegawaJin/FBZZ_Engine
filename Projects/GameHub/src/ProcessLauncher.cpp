// FBZZ Engine
// ProcessLauncher.cpp | fbzz::hub
// Launch Editor process
#include "ProcessLauncher.hpp"
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>

#include <Windows.h>
#include <array>
#include <filesystem>
#include <vector>

namespace fbzz::hub {

namespace {

namespace engine_util = fbzz::util;

bool Exists(const std::filesystem::path& path)
{
    return engine_util::FileSystem::Exists(path);
}

std::wstring Quote(const std::wstring& text)
{
    return L"\"" + text + L"\"";
}

} // namespace

bool ProcessLauncher::OpenInEditor(const HubConfig& config, const std::string& projectPath, std::string& errorMessage)
{
    const std::wstring editorPath = ResolveEditorPath(config);
    if (editorPath.empty() || !Exists(editorPath)) {
        errorMessage = "FBZZEditor.exe was not found. Build the editor launcher target or set it in Settings.";
        return false;
    }

    const std::wstring projectWide = engine_util::StringUtils::ToWide(projectPath);
    std::wstring commandLine = Quote(editorPath) + L" --project " + Quote(projectWide);
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);

    PROCESS_INFORMATION process{};
    const BOOL ok = CreateProcessW(
        editorPath.c_str(),
        mutableCommand.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startup,
        &process);

    if (!ok) {
        errorMessage = "Failed to launch the editor executable.";
        return false;
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

std::wstring ProcessLauncher::ResolveEditorPath(const HubConfig& config)
{
    if (!config.GetEditorExe().empty()) {
        return engine_util::StringUtils::ToWide(config.GetEditorExe());
    }

    const std::filesystem::path exeDir = engine_util::FileSystem::GetExecutableDirectory();
    const std::filesystem::path cwd = engine_util::FileSystem::GetCurrentDirectory();
    const std::string exeDirText = engine_util::FileSystem::PathToUtf8(exeDir);
    const bool isDebugHub = engine_util::StringUtils::ContainsCI(exeDirText, "/debug");
    const std::filesystem::path matchingBuildEditor = isDebugHub
        ? cwd / L"build" / L"debug" / L"Projects" / L"EditorLauncher" / L"Debug" / L"FBZZEditor.exe"
        : cwd / L"build" / L"release" / L"Projects" / L"EditorLauncher" / L"Release" / L"FBZZEditor.exe";

    const std::array<std::filesystem::path, 3> candidates = {
        exeDir / L"FBZZEditor.exe",
        exeDir.parent_path() / L"EditorLauncher" / L"FBZZEditor.exe",
        matchingBuildEditor
    };

    for (const auto& candidate : candidates) {
        if (Exists(candidate)) {
            return candidate.wstring();
        }
    }

    return (exeDir / L"FBZZEditor.exe").wstring();
}

} // namespace fbzz::hub
