/// @file    StandaloneEntry.cpp
/// @brief   Standalone の起動、ログとランタイム寿命を Engine 内へ閉じる。
/// @author  Hasegawa Jin
/// @date    2026-10-30
#include <Engine/Standalone.hpp>
#include <Engine/Asset/AssetSession.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/CrashHandler.hpp>
#include <Engine/Core/DeveloperMode.hpp>
#include <Engine/Core/ILogSink.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/ProjectResolver.hpp>
#include <Engine/ProjectSettings.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/SceneUtils.hpp>
#include <Engine/Scene/StandaloneProjectModule.hpp>
#include <Engine/Util/FileSystem.hpp>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

namespace fbzz {
namespace {

using fbzz::util::FileSystem;

/// @note 開発ビルドでは exe から最大 6 段親を遡って .fbzz_proj を探す。
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
/// @note --project を明示で渡す。
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

void OpenGameLog(FileLogSink& sink, const char* applicationName)
{
    sink.file = FileSystem::OpenBinaryWriter(FileSystem::GetExecutableDirectory() / L"game.log");
    if (!sink.file.is_open()) return;

    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char timeBuf[64] = {};
    ctime_s(timeBuf, sizeof(timeBuf), &now);
    sink.file << "=== " << applicationName << " Log === " << timeBuf;
    fbzz::core::Logger::AddSink(&sink);
}

/// @brief ProjectSettings からウィンドウ設定を構築して StandaloneProjectModule を起動する。
/// @note Application::Init() 前にウィンドウサイズを決定し、Renderer 初期化時点で正しいバックバッファを作る。
[[nodiscard]] int RunStandalone(fbzz::core::Application& app, const fbzz::LaunchProject& project)
{
    fbzz::ProjectSettings settings;
    if (!settings.Load(FileSystem::PathToUtf8(project.settingsFile))) {
        MessageBoxW(nullptr, L"Failed to load ProjectSettings.", L"FBZZ Runtime", MB_OK | MB_ICONERROR);
        return 1;
    }

    /// @note ProjectSettings の renderer 指定 (dx11/dx12) でレンダラーを生成する (--renderer= があれば Application::Init 内でそちらが優先)。
    if (!app.Init(fbzz::scene::MakeWindowConfig(settings), settings.app.rendererBackend)) return 1;

    auto& renderer = app.GetRenderer();
    fbzz::renderer::ResourceManager resources(renderer);
    app.SetResourceManager(&resources);
    fbzz::asset::AssetSession assets(resources, FileSystem::PathToUtf8(project.root / L"Assets") + "/");

    fbzz::scene::StandaloneProjectModule module(
        renderer, resources, project.root, project.sceneFile, settings);
    app.Run(module);
    return 0;
}

} /// @note namespace

int RunStandaloneApplication(const char* applicationName)
{
    if (!applicationName) return 1;
    const std::filesystem::path projectPath = ParseProjectPath();
    if (projectPath.empty()) {
        MessageBoxW(nullptr,
                    L"Project path was not specified and the project was not found.\n\ngame.exe --project <path>",
                    L"FBZZ Runtime", MB_OK | MB_ICONERROR);
        return 1;
    }

    fbzz::ProjectResolver resolver;
    if (!resolver.Resolve(projectPath)) {
        MessageBoxW(nullptr, resolver.ErrorMessage().c_str(), L"FBZZ Runtime", MB_OK | MB_ICONERROR);
        return 1;
    }
    const fbzz::LaunchProject& project = resolver.Get();

    /// @note 配布物は exe 隣に Assets があるため exeDir を CWD にする。Editor の Tools > Standalone から
    /// @note Binaries/Development の exe を起動する場合は Assets が project.root にあるため、相対 shader path が
    /// @note 解決できるよう CWD を切り替える。
    const std::filesystem::path executableDirectory = FileSystem::GetExecutableDirectory();
    const std::filesystem::path workingDirectory = FileSystem::Exists(executableDirectory / L".fbzz_proj")
        ? executableDirectory
        : project.root;
    SetCurrentDirectoryW(workingDirectory.wstring().c_str());

    FileLogSink logSink;
    OpenGameLog(logSink, applicationName);

    /// @note 終了処理で落ちても残すため Uninstall しない。配布物では exe の隣の Saved/Crashes に残る。
    fbzz::core::CrashHandler::Install(workingDirectory, applicationName);
    fbzz::core::CrashHandler::NotifyUnreported(workingDirectory, applicationName, true);
    fbzz::core::DeveloperMode::InitFromCommandLine();


    auto& app = fbzz::core::Application::Get();
    const int result = RunStandalone(app, project);

    app.Shutdown();
    fbzz::core::Logger::RemoveSink(&logSink);
    return result;
}

} /// @note namespace fbzz
