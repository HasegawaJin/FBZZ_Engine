/// @file    DeveloperMode.cpp
/// @brief   開発者モードの判定。起動引数・環境変数・設定の 3 つを合わせる。
/// @author  Hasegawa Jin
/// @date    2026-09-19
/// @see Docs/design/developer-mode.md
#include "Engine/Core/Platform/DeveloperMode.hpp"
#include "Engine/Core/Diagnostics/Logger.hpp"
#include "DeveloperModeLaunch.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <shellapi.h>

#include <iterator>
#include <string>

namespace fbzz::core {
namespace {

bool g_developerForcedByLaunch = false;
bool g_developerPreference     = false;

/// @brief 環境変数 FBZZ_DEVELOPER_MODE が "1" か。
/// @see https://learn.microsoft.com/en-us/windows/win32/api/processenv/nf-processenv-getenvironmentvariablew GetEnvironmentVariableW のバッファ長と変数が存在しない場合の規定。
bool DeveloperEnvironmentFlag()
{
    wchar_t value[8] = {};
    const DWORD length = GetEnvironmentVariableW(L"FBZZ_DEVELOPER_MODE", value, static_cast<DWORD>(std::size(value)));
    return length == 1 && value[0] == L'1';
}

} // namespace

bool detail::HasDeveloperLaunchFlag(std::wstring_view commandLine)
{
    // パスの中の --developer を拾わないよう、Windows の引数分割規則を使う。
    const std::wstring terminated(commandLine);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(terminated.c_str(), &argc);
    if (!argv) {
        return false;
    }
    bool found = false;
    for (int i = 1; i < argc && !found; ++i) {
        found = std::wstring_view(argv[i]) == L"--developer";
    }
    LocalFree(argv);
    return found;
}

void DeveloperMode::InitFromCommandLine()
{
    g_developerForcedByLaunch = detail::HasDeveloperLaunchFlag(GetCommandLineW()) || DeveloperEnvironmentFlag();
    if (g_developerForcedByLaunch)
        FBZZ_LOG_INFO("DeveloperMode: 起動引数か環境変数で有効にしました");
}

bool DeveloperMode::IsEnabled()
{
    return g_developerForcedByLaunch || g_developerPreference;
}

bool DeveloperMode::IsForcedByLaunch()
{
    return g_developerForcedByLaunch;
}

bool DeveloperMode::Preference()
{
    return g_developerPreference;
}

void DeveloperMode::SetPreference(bool enabled)
{
    g_developerPreference = enabled;
}

} // namespace fbzz::core
