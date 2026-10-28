/// @file    DeveloperModeLaunch.hpp
/// @brief   開発者モードの起動引数を Windows の引数分割規則で判定する内部契約。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#pragma once
#include <string_view>

namespace fbzz::core::detail {

/// @return 実行ファイル名を除いた独立した引数に --developer があれば true。
/// @see https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-commandlinetoargvw CommandLineToArgvW argument parsing and LocalFree ownership
[[nodiscard]] bool HasDeveloperLaunchFlag(std::wstring_view commandLine);

} /// @note namespace fbzz::core::detail
