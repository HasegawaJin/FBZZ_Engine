/// @file    PixLauncher.hpp
/// @brief   Installed PIX UI path validation and explicit external launch.
/// @author  Hasegawa Jin
/// @date    2026-10-02
#pragma once

#include <filesystem>
#include <string>

namespace fbzz::editor {

/// @note Only the verified stable installation is the ordinary-launch default; no executable search path is used.
[[nodiscard]] std::filesystem::path DefaultPixInstalledDirectory();

/// @return false leaves executable unchanged; selected directories never fall back to another installation.
[[nodiscard]] bool ResolvePixUiExecutable(const std::filesystem::path& installedDirectory,
                                         std::filesystem::path& executable,
                                         std::string& error);

/// @note Opens only PIX UI, without capture, injection, editor restart or project mutation.
/// @see https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw Explicit application name avoids executable search and whitespace ambiguity.
[[nodiscard]] bool LaunchPixUi(const std::filesystem::path& executable, std::string& error);

} /// @note namespace fbzz::editor
