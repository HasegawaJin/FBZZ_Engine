/// @file    StandaloneLauncher.cpp
/// @brief   CreateProcess による子プロセス起動の実装。
/// @author  Hasegawa Jin
/// @date    2026-05-31
#include <Editor/Util/StandaloneLauncher.hpp>

#include <Engine/Core/Logger.hpp>
#include <Engine/Util/StringUtils.hpp>

#include <Windows.h>
#include <string>

namespace fbzz::editor {

namespace {

using fbzz::util::StringUtils;

/// @brief CreateProcess を呼び出してプロセスを起動する汎用ヘルパー。
/// @note lpCommandLine は書き込み可能バッファを要求するため、wstring のコピーを data() で渡す。
bool SpawnProcess(std::wstring cmdLine, const std::wstring& workingDir)
{
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    const LPCWSTR workDir = workingDir.empty() ? nullptr : workingDir.c_str();

    const BOOL ok = CreateProcessW(
        nullptr,
        /// @note コピーを渡す (書き込み可能バッファ)
        cmdLine.data(),
        nullptr, nullptr,
        FALSE, 0,
        nullptr,
        workDir,
        &si, &pi);

    if (ok) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        FBZZ_LOG_ERROR("StandaloneLauncher: CreateProcess failed (error=%lu)", GetLastError());
    }
    return ok != FALSE;
}

} // namespace

bool StandaloneLauncher::LaunchExe(const std::string& exePath, const std::string& workingDir)
{
    /// @note ビルド済み配布版は引数なしで起動すると .fbzz_proj を自動検出して Standalone モードになる。
    std::wstring cmd = L"\"" + StringUtils::ToWide(exePath) + L"\"";
    return SpawnProcess(std::move(cmd), StringUtils::ToWide(workingDir));
}

} // namespace fbzz::editor
