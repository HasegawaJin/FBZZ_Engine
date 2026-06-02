// FBZZ Engine
// LaunchArgs.cpp | fbzz::sandbox
// Sandbox のコマンドライン引数解析
#include "LaunchArgs.hpp"

#include "Util/FileUtil.hpp"
#include "Util/PathUtil.hpp"

#include <Windows.h>
#include <shellapi.h>

namespace fbzz::sandbox {
namespace {

std::filesystem::path FindDefaultSandboxProjectPath()
{
    const std::filesystem::path executableProject = util::GetExecutableDirectory() / L"SandboxProject";
    if (util::Exists(executableProject / L".fbzz_proj")) {
        return executableProject;
    }

    std::filesystem::path current = util::GetExecutableDirectory();
    for (int i = 0; i < 8 && !current.empty(); ++i) {
        const std::filesystem::path candidate = current / L"Projects" / L"GameHub" / L"Templates" / L"standard";
        if (util::Exists(candidate / L".fbzz_proj")) {
            return candidate;
        }
        current = current.parent_path();
    }

    current = util::MakeAbsolute(std::filesystem::current_path());
    for (int i = 0; i < 8 && !current.empty(); ++i) {
        const std::filesystem::path candidate = current / L"Projects" / L"GameHub" / L"Templates" / L"standard";
        if (util::Exists(candidate / L".fbzz_proj")) {
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
        const std::filesystem::path exeDir = util::GetExecutableDirectory();
        if (util::Exists(exeDir / L".fbzz_proj")) {
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
