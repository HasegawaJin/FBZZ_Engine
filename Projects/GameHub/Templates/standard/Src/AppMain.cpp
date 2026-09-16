// {{PROJECT_NAME}}
// AppMain.cpp | {{CPP_NAMESPACE}}
// スタンドアロン実行のエントリポイント
//
// WHAT:
//   {{TARGET_NAME}}.exe --project <path>          -> 指定プロジェクトを起動
//   {{TARGET_NAME}}.exe (exe 隣に .fbzz_proj あり) -> 配布物として自分のプロジェクトを起動
//   {{TARGET_NAME}}.exe (引数なし)                -> 開発モード: 親を遡って .fbzz_proj を探す
//
// WHY: このターゲットは常に FBZZ_STANDALONE_TARGET 付きでビルドされ、Editor をリンクしない
//      (エディタは別バイナリ FBZZEditorLauncher)。起動分岐・ログ・CWD 決定だけを持ち、
//      ゲームループ本体は Engine の StandaloneProjectModule に委ねる。
//      ゲーム固有のスクリプト登録は GameMain.cpp の担当。
#include "{{TARGET_NAME}}/ProjectAPI.hpp"

#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/ILogSink.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/ProjectResolver.hpp>
#include <Engine/ProjectSettings.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/SceneUtils.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/StandaloneProjectModule.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <Windows.h>
#include <shellapi.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

namespace {{CPP_NAMESPACE}} {
namespace {

using fbzz::util::FileSystem;

/// exe から最大 6 段親を遡って .fbzz_proj を探す (開発ビルド用)。
std::filesystem::path FindDefaultProjectPath()
{
    std::filesystem::path dir = FileSystem::GetExecutableDirectory();
    for (int i = 0; i < 6 && !dir.empty(); ++i) {
        if (FileSystem::Exists(dir / L".fbzz_proj"))
            return dir;
        dir = dir.parent_path();
    }
    return {};
}

/// 起動対象のプロジェクトパスを決める。
/// WHY: 配布物は exe 隣の .fbzz_proj を、開発ビルドは親階層のプロジェクトを自動で拾う。
///      Editor の Tools > Standalone は --project を明示で渡す。
std::filesystem::path ParseProjectPath()
{
    std::filesystem::path projectPath;

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        // WHY: Editor は --standalone も渡してくるが、この exe は常に Standalone なので読み捨てる。
        for (int i = 1; i < argc; ++i) {
            if (std::wstring(argv[i]) == L"--project" && i + 1 < argc)
                projectPath = argv[++i];
        }
        LocalFree(argv);
    }

    if (projectPath.empty()) {
        const std::filesystem::path exeDir = FileSystem::GetExecutableDirectory();
        projectPath = FileSystem::Exists(exeDir / L".fbzz_proj") ? exeDir : FindDefaultProjectPath();
    }
    return projectPath;
}

/// ゲームログを exe 隣の game.log に書き出すシンク。
/// WHY: WIN32 サブシステムはコンソールがなく printf が見えない。
///      OutputDebugString はデバッガなしでは確認できないため、ファイルに残す。
struct FileLogSink final : fbzz::core::ILogSink {
    std::ofstream file;

    void OnLog(const fbzz::core::LogEntry& entry) override
    {
        if (!file.is_open()) return;
        const char* prefix = "";
        switch (entry.level) {
        case fbzz::core::LogLevel::DEBUG:     prefix = "[DEBUG] "; break;
        case fbzz::core::LogLevel::INFO:      prefix = "[INFO]  "; break;
        case fbzz::core::LogLevel::WARNING:   prefix = "[WARN]  "; break;
        case fbzz::core::LogLevel::LOG_ERROR: prefix = "[ERROR] "; break;
        }
        file << prefix << entry.message << '\n';
        file.flush();
    }
};

void OpenGameLog(FileLogSink& sink)
{
    sink.file = FileSystem::OpenBinaryWriter(FileSystem::GetExecutableDirectory() / L"game.log");
    if (!sink.file.is_open()) return;

    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char timeBuf[64] = {};
    ctime_s(timeBuf, sizeof(timeBuf), &now);
    sink.file << "=== {{PROJECT_NAME}} Log === " << timeBuf;
    fbzz::core::Logger::AddSink(&sink);
}

/// ProjectSettings からウィンドウ設定を構築して StandaloneProjectModule を起動する。
/// WHY: Application::Init() 前にウィンドウサイズを決定し、Renderer 初期化時点で正しいバックバッファを作る。
[[nodiscard]] int RunStandalone(fbzz::core::Application& app, const fbzz::LaunchProject& project)
{
    fbzz::ProjectSettings settings;
    if (!settings.Load(FileSystem::PathToUtf8(project.settingsFile))) {
        MessageBoxW(nullptr, L"Failed to load ProjectSettings.", L"{{PROJECT_NAME}}", MB_OK | MB_ICONERROR);
        return 1;
    }

    // WHY: ProjectSettings の renderer 指定 (dx11/dx12) でレンダラーを生成する
    //      (--renderer= があれば Application::Init 内でそちらが優先)。
    if (!app.Init(fbzz::scene::MakeWindowConfig(settings), settings.app.rendererBackend)) return 1;

    auto& renderer = app.GetRenderer();
    fbzz::renderer::ResourceManager resources(renderer);
    fbzz::asset::AssetManager::Init(resources, FileSystem::PathToUtf8(project.root / L"Assets") + "/");

    fbzz::scene::StandaloneProjectModule module(
        renderer, resources, project.root, project.sceneFile, settings);
    app.Run(module);
    return 0;
}

} // namespace

int Run()
{
    const std::filesystem::path projectPath = ParseProjectPath();
    if (projectPath.empty()) {
        MessageBoxW(nullptr,
                    L"Project path was not specified and the project was not found.\n\n{{TARGET_NAME}}.exe --project <path>",
                    L"{{PROJECT_NAME}}", MB_OK | MB_ICONERROR);
        return 1;
    }

    fbzz::ProjectResolver resolver;
    if (!resolver.Resolve(projectPath)) {
        MessageBoxW(nullptr, resolver.ErrorMessage().c_str(), L"{{PROJECT_NAME}}", MB_OK | MB_ICONERROR);
        return 1;
    }
    const fbzz::LaunchProject& project = resolver.Get();

    // WHY: 配布物は exe 隣に Assets があるため exeDir を CWD にする。
    //      Editor の Tools > Standalone から Binaries/Development の exe を起動する場合は
    //      Assets が project.root にあるため、相対 shader path が解決できるよう CWD を切り替える。
    const std::filesystem::path executableDirectory = FileSystem::GetExecutableDirectory();
    const std::filesystem::path workingDirectory = FileSystem::Exists(executableDirectory / L".fbzz_proj")
        ? executableDirectory
        : project.root;
    SetCurrentDirectoryW(workingDirectory.wstring().c_str());

    FileLogSink logSink;
    OpenGameLog(logSink);

    // WHY: SceneSerializer がシーンを復元するときに ScriptFactory を参照するため、
    //      シーンロードより前に呼ぶ必要がある。
    RegisterScripts();
    FBZZ_LOG_INFO("{{PROJECT_NAME}}: RegisterScripts complete (%d types registered)",
        static_cast<int>(fbzz::scene::ScriptFactory::RegisteredTypeNames().size()));

    auto& app = fbzz::core::Application::Get();
    const int result = RunStandalone(app, project);

    fbzz::asset::AssetManager::UnloadAll();
    app.Shutdown();
    fbzz::core::Logger::RemoveSink(&logSink);
    return result;
}

} // namespace {{CPP_NAMESPACE}}

int main()
{
    return {{CPP_NAMESPACE}}::Run();
}
