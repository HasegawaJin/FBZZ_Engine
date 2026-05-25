// FBZZ Engine
// ProcessLauncher.cpp | fbzz::hub
// Launch Editor process
#include "ProcessLauncher.hpp"

#include <Windows.h>
#include <array>
#include <filesystem>
#include <vector>

namespace fbzz::hub {

namespace {

std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty()) return {};

    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 0) {
        return std::filesystem::path(text).wstring();
    }

    std::wstring wide(static_cast<size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), size);
    return wide;
}

std::filesystem::path GetExecutableDirectory()
{
    wchar_t buffer[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(buffer).parent_path();
}

bool Exists(const std::filesystem::path& path)
{
    std::error_code ec;
    return std::filesystem::exists(path, ec);
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

    const std::wstring projectWide = Utf8ToWide(projectPath);
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
        return Utf8ToWide(config.GetEditorExe());
    }

    const std::filesystem::path exeDir = GetExecutableDirectory();
    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    const std::string exeDirText = exeDir.generic_string();
    const bool isDebugHub = exeDirText.find("/debug/") != std::string::npos
        || exeDirText.find("\\debug\\") != std::string::npos;
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
