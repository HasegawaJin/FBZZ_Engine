/// @file    main.cpp
/// @brief   エディタ / スタンドアロン両対応のエントリポイント。
/// @author  Hasegawa Jin
/// @date    2026-05-25
///
/// コマンドライン引数でエディタ / スタンドアロン / バッチを切り替える単一バイナリ。
/// `--project` 省略時は exe 隣の `.fbzz_proj` の有無で配布 Standalone か開発用テンプレート
/// Editor かを判定する (配布時は exe をリネームするだけでよい)。文字列・パス変換は
/// `Engine/Util` に集約し `EditorLauncher` と `Sandbox` の両方から使う。
#include "JobBreakaway.hpp"
#include "StandaloneApp.hpp"
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/CrashHandler.hpp>
#include <Engine/Core/DeveloperMode.hpp>
#include <Engine/Core/EngineRebuildBootstrap.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/ProjectResolver.hpp>
#include <Engine/ProjectSettings.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/SceneUtils.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <Editor/EditorApp.hpp>

#include <Windows.h>
#include <shellapi.h>
#include <filesystem>
#include <string>

namespace fbzz::editor_launcher {

namespace {

using fbzz::util::FileSystem;
using fbzz::util::StringUtils;

/// @brief コマンドライン引数の解析結果。
struct LaunchArgs {
    std::filesystem::path projectPath;
    std::filesystem::path scriptsDll;      ///< --scripts-dll で上書き指定 (省略可)
    bool                  standalone = false;
    /// @brief --batch のシナリオ。空でなければ Playtest を回して終了する (Docs/design/ai-verification-loop.md)。
    std::filesystem::path batchScenario;
    std::filesystem::path batchReport;     ///< --report
    bool                  updateBaselines = false;
    bool                  skipImages = false;
    bool                  hidden = false;  ///< --hidden: 窓を出さない
};

std::filesystem::path FindDefaultEditorProjectPath()
{
    /// @note `build/Release/Binaries/Release/FBZZEditor.exe` を直接起動する開発導線では
    ///       exe 隣に `.fbzz_proj` が存在しない。配布物と区別し、標準テンプレートを Editor で開く。
    std::filesystem::path current = FileSystem::GetExecutableDirectory();
    for (int i = 0; i < 8 && !current.empty(); ++i) {
        const std::filesystem::path candidate =
            current / L"Projects" / L"GameHub" / L"Templates" / L"standard";
        if (FileSystem::Exists(candidate / L".fbzz_proj")) {
            return candidate;
        }
        current = current.parent_path();
    }

    current = FileSystem::MakeAbsolute(std::filesystem::current_path());
    for (int i = 0; i < 8 && !current.empty(); ++i) {
        const std::filesystem::path candidate =
            current / L"Projects" / L"GameHub" / L"Templates" / L"standard";
        if (FileSystem::Exists(candidate / L".fbzz_proj")) {
            return candidate;
        }
        current = current.parent_path();
    }
    return {};
}

/// @brief コマンドライン引数を解析して `LaunchArgs` を返す。
/// @note 引数なし起動は exe 隣に `.fbzz_proj` があれば配布版 Standalone、なければ
///       標準テンプレートを Standalone として開く。`build_root` は未解決だが
///       `StandaloneApp` が exe 隣の `SandboxScripts.dll` へフォールバックする。
LaunchArgs ParseArgs()
{
    LaunchArgs args;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return args;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--project" && i + 1 < argc)
            args.projectPath = argv[++i];
        else if (arg == L"--scripts-dll" && i + 1 < argc)
            args.scriptsDll = argv[++i];
        else if (arg == L"--standalone")
            args.standalone = true;
        /// @note 相対パスは呼び出し側のカレントで解く。Run() は後でカレントをプロジェクトへ移す。
        else if (arg == L"--batch" && i + 1 < argc)
            args.batchScenario = FileSystem::MakeAbsolute(std::filesystem::path(argv[++i]));
        else if (arg == L"--report" && i + 1 < argc)
            args.batchReport = FileSystem::MakeAbsolute(std::filesystem::path(argv[++i]));
        else if (arg == L"--update-baselines")
            args.updateBaselines = true;
        else if (arg == L"--skip-images")
            args.skipImages = true;
        else if (arg == L"--hidden")
            args.hidden = true;
    }
    LocalFree(argv);

    if (args.projectPath.empty()) {
        const std::filesystem::path exeDir = FileSystem::GetExecutableDirectory();
        if (FileSystem::Exists(exeDir / L".fbzz_proj")) {
            args.projectPath = exeDir;
            args.standalone  = true;
        } else {
            args.projectPath = FindDefaultEditorProjectPath();
            args.standalone  = true;
        }
    }

    /// @note バッチは «エディターと同じ実装» で回すのが目的なので Standalone に倒さない。既定プロジェクトの補完より後に置く。
    if (!args.batchScenario.empty()) args.standalone = false;

    return args;
}

} // namespace

int Run()
{
    /// @note 起動元が「閉じたら配下ごと殺す」Job に自分を入れている場合、その外へ自分を
    ///       起動し直す。ウィンドウもプロジェクトも作る前なら、作り直しの副作用が無い。
    const LaunchArgs args = ParseArgs();
    /// @note バッチは終了コードが結果そのもの。起動し直すと呼び出し側が受け取るのは «0 で抜けた親» の値になる。
    const bool batch = !args.batchScenario.empty();

    if (!batch && RelaunchOutsideKillOnCloseJob())
        return 0;

    /// @note `FBZZEngine.dll` は実行中ロックされ再ビルドできない。Engine ソースが古い DLL より
    ///       新しければ、ここで一旦終了して cmake 再ビルド → 再起動を予約する (開発ビルドのみ)。
    if (!batch && fbzz::core::CheckEngineFreshnessAndRelaunch())
        return 0;

    fbzz::ProjectResolver resolver;
    if (!resolver.Resolve(args.projectPath)) {
        /// @note バッチでモーダルを出すと CI が人のクリックを待って固まる。
        if (batch) return 2;
        MessageBoxW(nullptr, resolver.ErrorMessage().c_str(), L"FBZZ", MB_OK | MB_ICONERROR);
        return 1;
    }
    fbzz::LaunchProject project = resolver.Get();
    /// @note `.fbzz_proj` に `scripts_dll` が書かれていないプロジェクト (DemoGame 等) では
    ///       `ProjectResolver` が `scriptsDll` を空のままにする。エディタから `--scripts-dll`
    ///       で解決済みパスが渡された場合はそれを優先して上書きする。
    if (!args.scriptsDll.empty() && project.scriptsDll.empty())
        project.scriptsDll = args.scriptsDll;

    const std::filesystem::path executableDirectory = FileSystem::GetExecutableDirectory();
    const std::filesystem::path workingDirectory = FileSystem::Exists(executableDirectory / L".fbzz_proj")
        ? executableDirectory
        : project.root;
    /// @note 配布物は exe 隣に Assets があるため exeDir を CWD にする。Editor から別プロジェクトを
    ///       `--project` 指定で Standalone 起動する場合は Assets が `project.root` にあるため、
    ///       相対 shader path が解決できるよう CWD を切り替える。
    SetCurrentDirectoryW(workingDirectory.wstring().c_str());

    /// @note 終了処理で落ちても残すため Uninstall しない。バッチは人が居ないので通知をダイアログにしない。
    /// @see Docs/design/crash-report.md
    const std::string crashAppName = args.standalone ? StringUtils::PathToUtf8(project.root.filename()) : "FBZZ Editor";
    core::CrashHandler::Install(workingDirectory, crashAppName);
    core::CrashHandler::NotifyUnreported(workingDirectory, crashAppName, !batch);
    core::DeveloperMode::InitFromCommandLine();

    auto& app = core::Application::Get();

    if (args.standalone) {
        /// @note Standalone モードではウィンドウを正しいサイズで生成するため
        ///       `Application::Init()` の前に `ProjectSettings` を読み込む必要がある。
        ///       Init 後に `Resize()` するとウィンドウが一瞬デフォルトサイズで表示されてしまう。
        ProjectSettings settings;
        if (!settings.Load(StringUtils::PathToUtf8(project.settingsFile))) {
            MessageBoxW(nullptr, L"ProjectSettings を読み込めませんでした。", L"FBZZ", MB_OK | MB_ICONERROR);
            return 1;
        }

        /// @note Standalone は `ProjectSettings` の renderer 指定でレンダラーを生成する。
        ///       コマンドライン `--renderer=` があれば `Application::Init` 内でそちらが優先される。
        if (!app.Init(scene::MakeWindowConfig(settings), settings.app.rendererBackend)) return 1;

        auto& renderer = app.GetRenderer();
        auto& imguiRenderer = app.GetImGuiRenderer();
        renderer::ResourceManager resources(renderer);
        asset::AssetManager::Init(resources, StringUtils::PathToUtf8(project.root / L"Assets") + "/");

        StandaloneApp standaloneApp(renderer, imguiRenderer, resources, project, settings);
        app.Run(standaloneApp);
        /// @note `ResourceManager` は app よりスコープが長いため `~ResourceManager()` が
        ///       `app::Shutdown()` より先に走る。事前に `Reset()` しないとデストラクタの
        ///       `LogLiveDebugResources` が GPU リソースを「外部保持」と誤判定して報告する。
        asset::AssetManager::UnloadAll();
        resources.Reset();
    } else {
        /// @note Editor も起動時プロジェクトの renderer 設定に従う。レンダラーはプロジェクト
        ///       読込前に生成するため設定をここで先読みしてバックエンドを渡す (`--renderer=`
        ///       があれば優先)。Load 失敗時は既定の DX12 で開く (本読込は `OpenProject` が行う)。
        ProjectSettings settings;
        settings.Load(StringUtils::PathToUtf8(project.settingsFile));
        if (!app.Init(core::Window::Config{}, settings.app.rendererBackend)) return 1;

        auto& renderer    = app.GetRenderer();
        auto& imguiRenderer = app.GetImGuiRenderer();
        renderer::ResourceManager resources(renderer);

        const std::filesystem::path assetRoot = project.root / L"Assets";
        asset::AssetManager::Init(resources, StringUtils::PathToUtf8(assetRoot) + "/");

        /// @note 隠すのは窓だけ。スワップチェーンと RT はそのまま作られるので、描画と撮影は通常起動と同じ経路を通る。
        if (args.hidden) ShowWindow(app.GetWindow().GetHandle(), SW_HIDE);

        editor::EditorApp editorApp;
        const int startupFailure = args.batchScenario.empty() ? 1 : 2;
        if (!editorApp.Init(renderer, imguiRenderer, resources, app.GetWindow())) return startupFailure;
        if (!editorApp.OpenProject(StringUtils::PathToUtf8(project.root),
                                   StringUtils::PathToUtf8(project.settingsFile),
                                   StringUtils::PathToUtf8(project.sceneFile))) return startupFailure;
        if (!args.batchScenario.empty()) {
            editor::EditorApp::BatchOptions batch;
            batch.scenarioPath = args.batchScenario;
            batch.reportPath = args.batchReport;
            batch.playtest.updateBaselines = args.updateBaselines;
            batch.playtest.skipImages = args.skipImages;
            editorApp.ConfigureBatch(std::move(batch));
        }
        app.Run(editorApp);
        const int exitCode = editorApp.BatchExitCode();
        /// @note standalone 側と同様、`~ResourceManager()` に先んじて GPU リソースを解放する。
        asset::AssetManager::UnloadAll();
        resources.Reset();
        app.Shutdown();
        return exitCode;
    }

    app.Shutdown();
    return 0;
}

} // namespace fbzz::editor_launcher

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    return fbzz::editor_launcher::Run();
}
