// FBZZ Engine
// StandaloneLauncher.cpp | fbzz::editor
// CreateProcess による子プロセス起動の実装
#include <Editor/Util/StandaloneLauncher.hpp>
#include <Windows.h>
#include <string>

namespace fbzz::editor {

namespace {

std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 0) return {};
    std::wstring wide(static_cast<size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), size);
    return wide;
}

// CreateProcess を呼び出してプロセスを起動する汎用ヘルパー。
// WHY: CreateProcess の lpCommandLine は書き込み可能バッファを要求するため、
//      wstring のコピーを data() で渡す。
bool SpawnProcess(std::wstring cmdLine, const std::wstring& workingDir)
{
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    const LPCWSTR workDir = workingDir.empty() ? nullptr : workingDir.c_str();

    const BOOL ok = CreateProcessW(
        nullptr,
        cmdLine.data(),   // コピーを渡す (書き込み可能バッファ)
        nullptr, nullptr,
        FALSE, 0,
        nullptr,
        workDir,
        &si, &pi);

    if (ok) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    return ok != FALSE;
}

} // namespace

bool StandaloneLauncher::Launch(const std::string& exePath, const std::string& projectPath)
{
    // コマンドライン: "<exePath>" --project "<projectPath>" --standalone
    std::wstring cmd =
        L"\"" + Utf8ToWide(exePath) + L"\""
        L" --project \"" + Utf8ToWide(projectPath) + L"\""
        L" --standalone";

    return SpawnProcess(std::move(cmd), {});
}

bool StandaloneLauncher::LaunchExe(const std::string& exePath, const std::string& workingDir)
{
    // WHY: ビルド済み配布版は引数なしで起動すると .fbzz_proj を自動検出して Standalone モードになる。
    std::wstring cmd = L"\"" + Utf8ToWide(exePath) + L"\"";
    return SpawnProcess(std::move(cmd), Utf8ToWide(workingDir));
}

} // namespace fbzz::editor
