/// @file    LaunchArgs.cpp
/// @brief   Sandbox のコマンドライン引数解析。
/// @author  Hasegawa Jin
/// @date    2026-06-02
#include "LaunchArgs.hpp"

#include <Engine/Util/FileSystem.hpp>

#include <Windows.h>
#include <shellapi.h>

namespace fbzz::sandbox {

using fbzz::util::FileSystem;

namespace {

std::filesystem::path FindDefaultSandboxProjectPath()
{
    const std::filesystem::path executableProject = FileSystem::GetExecutableDirectory() / L"SandboxProject";
    if (FileSystem::Exists(executableProject / L".fbzz_proj")) {
        return executableProject;
    }

    std::filesystem::path current = FileSystem::GetExecutableDirectory();
    for (int i = 0; i < 8 && !current.empty(); ++i) {
        const std::filesystem::path candidate = current / L"Projects" / L"GameHub" / L"Templates" / L"standard";
        if (FileSystem::Exists(candidate / L".fbzz_proj")) {
            return candidate;
        }
        current = current.parent_path();
    }

    current = FileSystem::MakeAbsolute(std::filesystem::current_path());
    for (int i = 0; i < 8 && !current.empty(); ++i) {
        const std::filesystem::path candidate = current / L"Projects" / L"GameHub" / L"Templates" / L"standard";
        if (FileSystem::Exists(candidate / L".fbzz_proj")) {
            return candidate;
        }
        current = current.parent_path();
    }

    return {};
}

} // namespace

LaunchArgs LaunchArgs::Parse()
{
    LaunchArgs args;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return args;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--project" && i + 1 < argc)
            args.projectPath = argv[++i];
        else if (arg == L"--standalone")
            args.standalone = true;
    }
    LocalFree(argv);

    if (args.projectPath.empty()) {
        const std::filesystem::path exeDir = FileSystem::GetExecutableDirectory();
        if (FileSystem::Exists(exeDir / L".fbzz_proj")) {
            // WHY: 配布 exe はプロジェクト直下に置かれるため、引数なしなら Standalone とみなす。
            args.projectPath = exeDir;
            args.standalone  = true;
        } else {
            // WHY: 開発時の引数なし起動ではテンプレートプロジェクトを Editor で開く。
            args.projectPath = FindDefaultSandboxProjectPath();
        }
    }

    return args;
}

} // namespace fbzz::sandbox
