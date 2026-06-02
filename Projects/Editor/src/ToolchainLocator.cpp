// FBZZ Engine
// ToolchainLocator.cpp | fbzz::editor
// RuntimeBuild が使用する CMake とビルド成果物パスの解決
#include <Editor/ToolchainLocator.hpp>
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fbzz::editor {
namespace {

std::filesystem::path GetSelfExePath()
{
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf);
}

std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), len);
    return out;
}

std::filesystem::path NormalizeConfigPath(const std::filesystem::path& configPath, const std::string& value)
{
    std::filesystem::path path(Utf8ToWide(value));
    if (!path.is_absolute())
        path = configPath.parent_path() / path;
    return path.lexically_normal();
}

std::filesystem::path FindBuildConfig()
{
    std::filesystem::path dir = GetSelfExePath().parent_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        const std::filesystem::path candidate = dir / L"build.config";
        if (std::filesystem::exists(candidate))
            return candidate;
        dir = dir.parent_path();
    }
    return {};
}

std::filesystem::path FindBuildConfigUnderRoot(const std::filesystem::path& buildRoot)
{
    if (buildRoot.empty()) return {};

    const std::filesystem::path direct = buildRoot / L"build.config";
    if (std::filesystem::exists(direct))
        return direct;

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(buildRoot, ec)) {
        if (!entry.is_directory(ec)) continue;
        const std::filesystem::path candidate = entry.path() / L"build.config";
        if (std::filesystem::exists(candidate))
            return candidate;
    }
    return {};
}

bool ReadBuildConfig(const std::filesystem::path& path, ToolchainLocator::Result& out)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;

    std::string line;
    while (std::getline(ifs, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "cmake_exe") {
            out.cmakeExe = NormalizeConfigPath(path, value);
        } else if (key == "build_dir") {
            out.buildDir = NormalizeConfigPath(path, value);
        } else if (key == "exe_debug") {
            out.exeDebug = NormalizeConfigPath(path, value);
        } else if (key == "exe_release") {
            out.exeRelease = NormalizeConfigPath(path, value);
        } else if (key == "scripts_dll_debug") {
            out.scriptsDllDebug = NormalizeConfigPath(path, value);
        } else if (key == "scripts_dll_release") {
            out.scriptsDllRelease = NormalizeConfigPath(path, value);
        }
    }
    return !out.buildDir.empty() && !out.exeDebug.empty() && !out.exeRelease.empty();
}

std::filesystem::path FindCMakeOnPath()
{
    wchar_t buffer[MAX_PATH]{};
    const DWORD result = SearchPathW(nullptr, L"cmake.exe", nullptr, MAX_PATH, buffer, nullptr);
    return result == 0 ? std::filesystem::path{} : std::filesystem::path(buffer);
}

std::filesystem::path FindVisualStudioCMake()
{
    wchar_t programFilesX86[MAX_PATH]{};
    const DWORD len = GetEnvironmentVariableW(L"ProgramFiles(x86)", programFilesX86, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};

    const std::filesystem::path vswhere =
        std::filesystem::path(programFilesX86) / L"Microsoft Visual Studio" / L"Installer" / L"vswhere.exe";
    if (!std::filesystem::exists(vswhere)) return {};

    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE readPipe = INVALID_HANDLE_VALUE;
    HANDLE writePipe = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return {};
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    std::wstring command = L"\"" + vswhere.wstring() + L"\" -latest -products * -property installationPath";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (!ok) {
        CloseHandle(readPipe);
        return {};
    }

    const DWORD waitResult = WaitForSingleObject(pi.hProcess, 3000);
    if (waitResult != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(readPipe);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return {};
    }
    std::string output;
    char buffer[512]{};
    DWORD read = 0;
    while (ReadFile(readPipe, buffer, sizeof(buffer) - 1, &read, nullptr) && read > 0)
        output.append(buffer, read);

    CloseHandle(readPipe);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    while (!output.empty() && (output.back() == '\r' || output.back() == '\n'))
        output.pop_back();
    if (output.empty()) return {};

    const std::filesystem::path cmake =
        std::filesystem::path(Utf8ToWide(output)) /
        L"Common7" / L"IDE" / L"CommonExtensions" / L"Microsoft" / L"CMake" / L"CMake" / L"bin" / L"cmake.exe";
    return std::filesystem::exists(cmake) ? cmake : std::filesystem::path{};
}

} // namespace

ToolchainLocator::Result ToolchainLocator::Locate(const std::filesystem::path& buildRoot)
{
    Result result;
    std::filesystem::path configPath;
    if (!buildRoot.empty()) {
        configPath = FindBuildConfigUnderRoot(buildRoot);
    }
    if (configPath.empty() || !std::filesystem::exists(configPath)) {
        configPath = FindBuildConfig();
    }
    if (!configPath.empty() && ReadBuildConfig(configPath, result)) {
        if (result.cmakeExe.empty() || !std::filesystem::exists(result.cmakeExe))
            result.cmakeExe = FindCMakeOnPath();
        if (result.cmakeExe.empty() || !std::filesystem::exists(result.cmakeExe))
            result.cmakeExe = FindVisualStudioCMake();

        result.found = !result.cmakeExe.empty()
                    && std::filesystem::exists(result.cmakeExe)
                    && std::filesystem::exists(result.buildDir);
        if (!result.found)
            result.error = "build.config は見つかりましたが cmake.exe または build_dir を解決できません。";
        return result;
    }

    result.cmakeExe = FindCMakeOnPath();
    if (result.cmakeExe.empty())
        result.cmakeExe = FindVisualStudioCMake();
    result.found = false;
    result.error = "build.config が見つかりません。Visual Studio / VS Code CMake Tools で Configure してください。";
    return result;
}

} // namespace fbzz::editor
