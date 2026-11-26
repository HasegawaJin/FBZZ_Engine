/// @file    LaunchArgs.cpp
/// @brief   Sandbox のコマンドライン引数解析。
/// @author  Hasegawa Jin
/// @date    2026-06-02
#include "LaunchArgs.hpp"

#include <Engine/Util/Files/FileSystem.hpp>

#include <Windows.h>
#include <shellapi.h>

namespace fbzz::sandbox {

using fbzz::util::FileSystem;

namespace {

constexpr int TEMPLATE_SEARCH_DEPTH = 8;

/// @brief 指定したディレクトリから親をたどり、開発用の標準プロジェクトを探す。
/// @param[in] directory 探索を開始するディレクトリ。
/// @return プロジェクトマーカーがあるディレクトリ。見つからなければ空。
std::filesystem::path FindTemplateProject(std::filesystem::path directory)
{
    // 指定されたディレクトリから親をたどり、開発ツリー内の標準テンプレートを探す。
    for (int depth = 0; depth < TEMPLATE_SEARCH_DEPTH && !directory.empty(); ++depth) {
        const std::filesystem::path candidate = directory / L"Projects" / L"GameHub" / L"Templates" / L"standard";
        if (FileSystem::Exists(candidate / L".fbzz_proj")) {
            return candidate;
        }
        directory = directory.parent_path();
    }

    return {};
}

/// @brief 配布用サンプル、実行ファイルの親、作業ディレクトリの親の順に起動先を探す。
/// @param[in] executableDirectory 実行ファイルがあるディレクトリ。
/// @return 最初に見つかったプロジェクトのディレクトリ。すべて見つからなければ空。
std::filesystem::path FindDefaultSandboxProjectPath(const std::filesystem::path& executableDirectory)
{
    // 配布用に実行ファイルの隣へ配置されたサンプルを優先する。
    const std::filesystem::path executableProject = executableDirectory / L"SandboxProject";
    if (FileSystem::Exists(executableProject / L".fbzz_proj")) {
        return executableProject;
    }

    // ビルド出力から開発ツリーを探せない場合は、起動時の作業ディレクトリも調べる。
    const std::filesystem::path executableTemplate = FindTemplateProject(executableDirectory);
    if (!executableTemplate.empty()) {
        return executableTemplate;
    }

    return FindTemplateProject(FileSystem::MakeAbsolute(std::filesystem::current_path()));
}

} // namespace

LaunchArgs LaunchArgs::Parse()
{
    LaunchArgs args;
    int argumentCount = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (!arguments) {
        return args;
    }

    // 先頭の実行ファイル名を飛ばし、プロジェクトパスを読んだらその引数も消費する。
    for (int argumentIndex = 1; argumentIndex < argumentCount; ++argumentIndex) {
        const std::wstring argument = arguments[argumentIndex];
        if (argument == L"--project") {
            if (argumentIndex + 1 < argumentCount) {
                ++argumentIndex;
                args.projectPath = arguments[argumentIndex];
            }
            continue;
        }
        if (argument == L"--standalone") {
            args.standalone = true;
        }
    }
    LocalFree(arguments);

    if (!args.projectPath.empty()) {
        return args;
    }

    // 配布時は実行ファイルとプロジェクトのマーカーが同じディレクトリにある。
    const std::filesystem::path executableDirectory = FileSystem::GetExecutableDirectory();
    if (FileSystem::Exists(executableDirectory / L".fbzz_proj")) {
        args.projectPath = executableDirectory;
        args.standalone = true;
        return args;
    }

    // 開発時は明示された起動モードを保ち、探索したサンプルを使用する。
    args.projectPath = FindDefaultSandboxProjectPath(executableDirectory);
    return args;
}

} // namespace fbzz::sandbox
