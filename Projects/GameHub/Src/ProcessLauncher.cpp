// FBZZ Engine
// ProcessLauncher.cpp | fbzz::hub
// CreateProcess による Editor 起動
#include "ProcessLauncher.hpp"

#include <Windows.h>
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

std::wstring Quote(const std::wstring& text)
{
    return L"\"" + text + L"\"";
}

} // namespace

bool ProcessLauncher::OpenInEditor(const HubConfig& config, const std::string& projectPath, std::string& errorMessage)
{
    const std::wstring editorPath = ResolveEditorPath(config);
    if (editorPath.empty() || !std::filesystem::exists(editorPath)) {
        errorMessage = "FBZZEditor.exe が見つかりません。Settings で設定してください。";
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
        CREATE_NEW_PROCESS_GROUP,
        nullptr,
        nullptr,
        &startup,
        &process);

    if (!ok) {
        errorMessage = "FBZZEditor.exe の起動に失敗しました。";
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

    return (GetExecutableDirectory() / L"FBZZEditor.exe").wstring();
}

} // namespace fbzz::hub
