/// @file    PixLauncher.cpp
/// @brief   Validated absolute WinPix.exe launch through a private Win32 boundary.
/// @author  Hasegawa Jin
/// @date    2026-10-02
#include "PixLauncher.hpp"
#include <Engine/Core/PixCapture.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <system_error>

namespace fbzz::editor {

std::filesystem::path DefaultPixInstalledDirectory()
{
    const DWORD size = GetEnvironmentVariableW(L"ProgramFiles", nullptr, 0);
    if (size == 0) return {};
    std::wstring programFiles(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(L"ProgramFiles", programFiles.data(), size);
    if (written == 0 || written >= size) return {};
    programFiles.resize(written);
    return std::filesystem::path(programFiles) / L"Microsoft PIX"
        / std::wstring(core::PIX_CAPTURE_STABLE_VERSION);
}

bool ResolvePixUiExecutable(const std::filesystem::path& installedDirectory,
                            std::filesystem::path& executable, std::string& error)
{
    error.clear();
    if (installedDirectory.empty() || !installedDirectory.is_absolute()) {
        error = "PIX installation must be an absolute directory.";
        return false;
    }
    std::error_code ec;
    const auto directory = std::filesystem::canonical(installedDirectory, ec);
    if (ec || !std::filesystem::is_directory(directory, ec) || ec) {
        error = "PIX installation directory is unavailable.";
        return false;
    }
    const auto candidate = std::filesystem::canonical(directory / L"WinPix.exe", ec);
    if (ec || !std::filesystem::is_regular_file(candidate, ec) || ec
        || candidate.parent_path() != directory) {
        error = "WinPix.exe is unavailable in the selected PIX installation.";
        return false;
    }
    executable = candidate;
    return true;
}

bool LaunchPixUi(const std::filesystem::path& executable, std::string& error)
{
    std::filesystem::path validated;
    if (!ResolvePixUiExecutable(executable.parent_path(), validated, error)
        || validated != executable) {
        if (error.empty()) error = "PIX UI executable must be its validated absolute path.";
        return false;
    }

    std::wstring command = L"\"" + validated.wstring() + L"\"";
    const auto workingDirectory = validated.parent_path().wstring();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_SHOWNORMAL;
    PROCESS_INFORMATION process{};
    /// @note No shell or argument forwarding: opening PIX cannot silently attach to or restart the Editor.
    if (!CreateProcessW(validated.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, workingDirectory.c_str(), &startup, &process)) {
        error = "PIX UI launch failed (Win32 error " + std::to_string(GetLastError()) + ").";
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    error.clear();
    return true;
}

} /// @note namespace fbzz::editor
