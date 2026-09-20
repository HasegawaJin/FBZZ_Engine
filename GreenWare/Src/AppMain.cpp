/// @file    AppMain.cpp
/// @brief   GreenWare スタンドアロン実行のエントリポイント
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// @note 起動モード: `GreenWare.exe --project <path>` は指定プロジェクトを起動、exe 隣に `.fbzz_proj` があれば
///       配布物として自分のプロジェクトを起動、引数なしなら親を遡って `.fbzz_proj` を探す開発モード。
/// @note このターゲットは常に FBZZ_STANDALONE_TARGET 付きでビルドされ、Editor をリンクしない (エディタは別バイナリ
///       FBZZEditorLauncher)。起動分岐・ログ・CWD 決定だけを持ち、ゲームループ本体は StandaloneProjectModule に委ね、
///       ゲーム固有のスクリプト登録は GameMain.cpp が担当する。
#include "GreenWare/ProjectAPI.hpp"

#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/CrashHandler.hpp>
#include <Engine/Core/DeveloperMode.hpp>
#include <Engine/Core/EngineRebuildBootstrap.hpp>
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

namespace greenware {
namespace {

using fbzz::util::FileSystem;

/// @brief exe から最大 6 段親を遡って .fbzz_proj を探す (開発ビルド用)。
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

/// @brief 起動対象のプロジェクトパスを決める。
/// @note 配布物は exe 隣の `.fbzz_proj` を、開発ビルドは親階層のプロジェクトを自動で拾う。Editor の Tools > Standalone は
///       --project を明示で渡す。
std::filesystem::path ParseProjectPath()
{
    std::filesystem::path projectPath;

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        /// @note Editor は --standalone も渡してくるが、この exe は常に Standalone なので読み捨てる。
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

/// @brief ゲームログを exe 隣の game.log に書き出すシンク。
/// @note WIN32 サブシステムはコンソールがなく printf が見えない。OutputDebugString もデバッガなしでは確認できないため、ファイルに残す。
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
    sink.file << "=== GreenWare Log === " << timeBuf;
    fbzz::core::Logger::AddSink(&sink);
}

/// @brief ProjectSettings からウィンドウ設定を構築して StandaloneProjectModule を起動する。
/// @note Application::Init() 前にウィンドウサイズを決定し、Renderer 初期化時点で正しいバックバッファを作る。
[[nodiscard]] int RunStandalone(fbzz::core::Application& app, const fbzz::LaunchProject& project)
{
    fbzz::ProjectSettings settings;
    if (!settings.Load(FileSystem::PathToUtf8(project.settingsFile))) {
        MessageBoxW(nullptr, L"Failed to load ProjectSettings.", L"GreenWare", MB_OK | MB_ICONERROR);
        return 1;
    }

    /// @note ProjectSettings の renderer 指定でレンダラーを生成する (--renderer= があれば Application::Init 内でそちらが優先)。
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
    /// @note FBZZEngine.dll は実行中ロックされ再ビルドできない。Engine ソースが古い DLL より新しければ、
    ///       ここで一旦終了して cmake 再ビルド → 再起動を予約する (開発ビルドのみ)。
    if (fbzz::core::CheckEngineFreshnessAndRelaunch())
        return 0;

    const std::filesystem::path projectPath = ParseProjectPath();
    if (projectPath.empty()) {
        MessageBoxW(nullptr,
                    L"Project path was not specified and the project was not found.\n\nGreenWare.exe --project <path>",
                    L"GreenWare", MB_OK | MB_ICONERROR);
        return 1;
    }

    fbzz::ProjectResolver resolver;
    if (!resolver.Resolve(projectPath)) {
        MessageBoxW(nullptr, resolver.ErrorMessage().c_str(), L"GreenWare", MB_OK | MB_ICONERROR);
        return 1;
    }
    const fbzz::LaunchProject& project = resolver.Get();

    /// @note 配布物は exe 隣に Assets があるため exeDir を CWD にする。Editor の Tools > Standalone から
    ///       Binaries/Development の exe を起動する場合は Assets が project.root にあるため、相対 shader path が
    ///       解決できるよう CWD を切り替える。
    const std::filesystem::path executableDirectory = FileSystem::GetExecutableDirectory();
    const std::filesystem::path workingDirectory = FileSystem::Exists(executableDirectory / L".fbzz_proj")
        ? executableDirectory
        : project.root;
    SetCurrentDirectoryW(workingDirectory.wstring().c_str());

    FileLogSink logSink;
    OpenGameLog(logSink);

    /// @note 終了処理で落ちても残すため Uninstall しない。
    /// @see Docs/design/crash-report.md
    fbzz::core::CrashHandler::Install(workingDirectory, "GreenWare");
    fbzz::core::CrashHandler::NotifyUnreported(workingDirectory, "GreenWare", true);
    fbzz::core::DeveloperMode::InitFromCommandLine();

    /// @note SceneSerializer がシーンを復元するときに ScriptFactory を参照するため、シーンロードより前に呼ぶ必要がある。
    RegisterScripts();
    FBZZ_LOG_INFO("GreenWare: RegisterScripts complete (%d types registered)",
        static_cast<int>(fbzz::scene::ScriptFactory::RegisteredTypeNames().size()));

    auto& app = fbzz::core::Application::Get();
    const int result = RunStandalone(app, project);

    fbzz::asset::AssetManager::UnloadAll();
    app.Shutdown();
    fbzz::core::Logger::RemoveSink(&logSink);
    return result;
}

} // namespace greenware

int main()
{
    return greenware::Run();
}
