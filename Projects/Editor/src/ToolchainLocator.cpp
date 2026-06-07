// FBZZ Engine
// ToolchainLocator.cpp | fbzz::editor
// RuntimeBuild が使用する CMake とビルド成果物パスの解決
#include <Editor/ToolchainLocator.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <Windows.h>
#include <filesystem>
#include <sstream>
#include <string>

namespace fbzz::editor {
namespace {

std::filesystem::path NormalizeConfigPath(const std::filesystem::path& configPath, const std::string& value)
{
    std::filesystem::path path = util::FileSystem::PathFromUtf8(value);
    if (!path.is_absolute())
        path = configPath.parent_path() / path;
    return path.lexically_normal();
}

// buildRoot 直下、または 1 段下のサブディレクトリから build.config を探す。
// WHY: cmake --preset fbzz-vs は build.config を buildRoot/VS/build.config に生成する。
//      直下にも置けるよう両方を試す。
std::filesystem::path FindBuildConfigUnderRoot(const std::filesystem::path& buildRoot)
{
    if (buildRoot.empty()) return {};

    const std::filesystem::path direct = buildRoot / L"build.config";
    if (util::FileSystem::Exists(direct))
        return direct;

    for (const auto& dir : util::FileSystem::ListDirectories(buildRoot)) {
        const std::filesystem::path candidate = dir / L"build.config";
        if (util::FileSystem::Exists(candidate))
            return candidate;
    }
    return {};
}

bool ReadBuildConfig(const std::filesystem::path& path, ToolchainLocator::Result& out)
{
    std::string text;
    if (!util::FileSystem::ReadText(path, text)) return false;

    std::string line;
    std::istringstream lines(text);
    while (std::getline(lines, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        const std::string key = util::StringUtils::Trim(line.substr(0, eq));
        const std::string value = util::StringUtils::Trim(line.substr(eq + 1));
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
    if (!util::FileSystem::Exists(vswhere)) return {};

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
        util::FileSystem::PathFromUtf8(output) /
        L"Common7" / L"IDE" / L"CommonExtensions" / L"Microsoft" / L"CMake" / L"CMake" / L"bin" / L"cmake.exe";
    return util::FileSystem::Exists(cmake) ? cmake : std::filesystem::path{};
}

} // namespace

ToolchainLocator::Result ToolchainLocator::Locate(const std::filesystem::path& buildRoot)
{
    Result result;

    if (buildRoot.empty()) {
        result.error = "build_root が .fbzz_proj に設定されていません。"
                       "プロジェクトを GameHub から開き直すか、.fbzz_proj に build_root を追記してください。";
        return result;
    }

    const std::filesystem::path configPath = FindBuildConfigUnderRoot(buildRoot);
    if (configPath.empty() || !util::FileSystem::Exists(configPath)) {
        result.error = "build.config が " + util::FileSystem::PathToUtf8(buildRoot)
                     + " 内に見つかりません。cmake --preset fbzz-vs を実行して Configure してください。";
        return result;
    }

    if (!ReadBuildConfig(configPath, result)) {
        result.error = "build.config の解析に失敗しました (build_dir / exe_debug / exe_release が未設定): "
                     + util::FileSystem::PathToUtf8(configPath);
        return result;
    }

    if (result.cmakeExe.empty() || !util::FileSystem::Exists(result.cmakeExe))
        result.cmakeExe = FindCMakeOnPath();
    if (result.cmakeExe.empty() || !util::FileSystem::Exists(result.cmakeExe))
        result.cmakeExe = FindVisualStudioCMake();

    result.found = !result.cmakeExe.empty()
                && util::FileSystem::Exists(result.cmakeExe)
                && util::FileSystem::Exists(result.buildDir);
    if (!result.found) {
        result.error = "build.config は見つかりましたが cmake.exe または build_dir を解決できません。";
        FBZZ_LOG_WARN("ToolchainLocator: %s", result.error.c_str());
    } else {
        FBZZ_LOG_DEBUG("ToolchainLocator: cmake=%s build=%s exe_debug=%s",
            util::FileSystem::PathToUtf8(configPath).c_str(),
            util::FileSystem::PathToUtf8(result.buildDir).c_str(),
            util::FileSystem::PathToUtf8(result.exeDebug).c_str());
    }

    return result;
}

} // namespace fbzz::editor
