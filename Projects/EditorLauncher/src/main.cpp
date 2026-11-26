/// @file    main.cpp
/// @brief   エディタ / スタンドアロン両対応のエントリポイント。
/// @author  Hasegawa Jin
/// @date    2026-05-25

/// @note コマンドライン引数で Editor / Standalone / バッチを切り替える。
/// @note `--project` 省略時は exe 隣の `.fbzz_proj` の有無で配布 Standalone か開発用テンプレート Editor かを判定する。
/// @note 文字列・パス変換は `Engine/Util` に集約し `EditorLauncher` と `Sandbox` の両方から使う。
#include "JobBreakaway.hpp"
#include "StandaloneApp.hpp"
#include "EditorPlayLog.hpp"
#include <Engine/Scene/Runtime/StandaloneProjectModule.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Core/Platform/Application.hpp>
#include <Engine/Core/Diagnostics/CrashHandler.hpp>
#include <Engine/Core/Platform/Cursor.hpp>
#include <Engine/Core/Platform/DeveloperMode.hpp>
#include <Engine/Core/Platform/EngineRebuildBootstrap.hpp>
#include <Engine/Core/Diagnostics/Logger.hpp>
#include <Engine/Core/Diagnostics/PixCapture.hpp>
#include <Engine/Project/ProjectResolver.hpp>
#include <Engine/Project/ProjectSettings.hpp>
#include <Graphics/Renderer/Resources/ResourceManager.hpp>
#include <Engine/Scene/Hierarchy/SceneUtils.hpp>
#include <Engine/Util/Files/FileSystem.hpp>
#include <Engine/Util/Text/StringUtils.hpp>
#include <Editor/EditorApp.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#ifdef _DEBUG
#include <crtdbg.h>
#endif

namespace fbzz::editor_launcher {

namespace {

using fbzz::util::FileSystem;
using fbzz::util::StringUtils;

/// @brief コマンドライン引数の解析結果。
struct LaunchArgs {
    std::filesystem::path projectPath; ///< 開くプロジェクト。空なら exe の配置場所から解決する。
    bool standalone = false; ///< スタンドアロンのゲームとして起動するか。
    bool editorPlay = false; ///< Editor が所有する子プロセスとして Play を実行するか。
    bool allowPhysicalInput = true; ///< Play 中に実機のマウス・キーボード入力を受け付けるか。
    std::filesystem::path playScene; ///< Editor Play がディスクから開くシーンの絶対パス。
    std::filesystem::path playLog; ///< Editor Play の起動診断と実行ログの出力先。
    std::filesystem::path batchScenario; ///< バッチで実行する Playtest シナリオ。空なら通常起動。
    std::filesystem::path batchReport; ///< バッチのレポートの出力先。
    bool updateBaselines = false; ///< Playtest の基準画像を更新するか。
    bool skipImages = false; ///< Playtest の画像比較を省くか。
    bool hidden = false; ///< バッチのウィンドウを表示せず起動するか。
    bool enableViewports = true; ///< Editor の Scene View と Game View を描画するか。
    core::PixCaptureOptions pixCapture; ///< PIX を初期化するために解析した起動オプション。
    std::string argumentError; ///< 起動できない引数の組み合わせを利用者へ伝える診断。
};

std::filesystem::path FindDefaultEditorProjectPath()
{
    // `build/Release/Binaries/Release/FBZZEditor.exe` を直接起動する開発導線では
    // exe 隣に `.fbzz_proj` が存在しない。配布物と区別し、標準テンプレートを Editor で開く。
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
/// @note 標準テンプレートも Standalone として起動する。
LaunchArgs ParseArgs()
{
    LaunchArgs args;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return args;
    }

    // 通常の起動引数と PIX に渡す引数を仕分ける。パスは現在の作業ディレクトリを基準にする。
    std::vector<std::wstring_view> pixArguments;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        const bool hasOperand = i + 1 < argc;
        if (arg == L"--project" && hasOperand) {
            args.projectPath = argv[++i];
        } else if (arg == L"--standalone") {
            args.standalone = true;
        } else if (arg == L"--editor-play") {
            args.editorPlay = true;
        } else if (arg == L"--play-no-physical-input") {
            args.allowPhysicalInput = false;
        } else if (arg == L"--play-scene" || arg == L"--play-log") {
            if (!hasOperand) {
                args.argumentError = "Missing Standalone Play path operand.";
                continue;
            }
            const std::filesystem::path path(argv[++i]);
            if (arg == L"--play-scene") {
                args.playScene = path;
            } else {
                args.playLog = path;
            }
        } else if (arg == L"--batch" && hasOperand) {
            args.batchScenario = FileSystem::MakeAbsolute(std::filesystem::path(argv[++i]));
        } else if (arg == L"--report" && hasOperand) {
            args.batchReport = FileSystem::MakeAbsolute(std::filesystem::path(argv[++i]));
        } else if (arg == L"--update-baselines") {
            args.updateBaselines = true;
        } else if (arg == L"--skip-images") {
            args.skipImages = true;
        } else if (arg == L"--hidden") {
            args.hidden = true;
        } else if (arg == L"--no-viewports") {
            args.enableViewports = false;
        } else if (arg == L"--pix-path") {
            pixArguments.emplace_back(argv[i]);
            if (hasOperand) {
                pixArguments.emplace_back(argv[++i]);
            }
        } else if (arg == L"--pix-capture" || arg.starts_with(L"--pix-path=")
                 || arg.starts_with(L"--pix-capture=")) {
            pixArguments.emplace_back(argv[i]);
        }
    }

    // 通常の起動引数を消費してから PIX の引数を解釈し、プロジェクトパスを DLL 指定と誤認しないようにする。
    if (!args.argumentError.empty()) {
        LocalFree(argv);
        return args;
    }
    if (!core::ParsePixCaptureOptions(pixArguments, args.pixCapture, args.argumentError)) {
        LocalFree(argv);
        return args;
    }
    LocalFree(argv);

    // プロジェクトを省略した場合は配布物の配置を優先し、見つからなければ開発用テンプレートを探す。
    if (args.projectPath.empty()) {
        const std::filesystem::path exeDir = FileSystem::GetExecutableDirectory();
        if (FileSystem::Exists(exeDir / L".fbzz_proj")) {
            args.projectPath = exeDir;
        } else {
            args.projectPath = FindDefaultEditorProjectPath();
        }
        args.standalone = true;
    }

    // Editor が起動した Play では、作業ディレクトリを変更してもシーンとログの参照先がぶれないようにする。
    if (args.editorPlay) {
        const bool validPlayMode = args.standalone && args.batchScenario.empty() && !args.hidden
            && !args.pixCapture.requested;
        const auto hasAbsolutePlayPaths = [&] {
            return args.projectPath.is_absolute() && args.playScene.is_absolute() && args.playLog.is_absolute();
        };
        if (!validPlayMode || !hasAbsolutePlayPaths()) {
            args.argumentError = "Editor Play requires --standalone and absolute --project, --play-scene and --play-log paths.";
        }
    } else if (!args.playScene.empty() || !args.playLog.empty() || !args.allowPhysicalInput) {
        args.argumentError = "--play-scene, --play-log and --play-no-physical-input require --editor-play.";
    }

    // バッチは «エディターと同じ実装» で回すのが目的なので Standalone に倒さない。既定プロジェクトの補完より後に置く。
    if (!args.batchScenario.empty()) {
        args.standalone = false;
    }
    if (args.standalone && !args.enableViewports) {
        args.argumentError = "--no-viewports requires Editor mode.";
    }

    return args;
}

} // namespace

int Run()
{
    // 起動元が「閉じたら配下ごと殺す」Job に自分を入れている場合、その外へ自分を
    // 起動し直す。ウィンドウもプロジェクトも作る前なら、作り直しの副作用が無い。
    const LaunchArgs args = ParseArgs();
    // バッチは終了コードが結果そのもの。起動し直すと呼び出し側が受け取るのは «0 で抜けた親» の値になる。
    const bool batch = !args.batchScenario.empty();
    if (args.hidden) {
        // CI はダイアログに応答できない。強制終了時も直前の診断を残す。
        /// @see Projects/Tests/TestKit/src/Output/Console.cpp SuppressBlockingErrorDialogs
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        _set_error_mode(_OUT_TO_STDERR);
        _set_abort_behavior(_WRITE_ABORT_MSG, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG
        for (const int reportType : { _CRT_ASSERT, _CRT_ERROR, _CRT_WARN }) {
            _CrtSetReportMode(reportType, _CRTDBG_MODE_FILE);
            _CrtSetReportFile(reportType, _CRTDBG_FILE_STDERR);
        }
#endif
    }
    if (!args.argumentError.empty()) {
        FBZZ_LOG_ERROR("EditorLauncher: %s", args.argumentError.c_str());
        return batch ? 2 : 1;
    }

    // バッチの終了コード、Editor が追跡する子プロセス ID、PIX の早期ロードを維持できる起動だけを再起動する。
    const bool canRelaunch = !batch && !args.editorPlay && !args.pixCapture.requested;
    if (canRelaunch && RelaunchOutsideKillOnCloseJob()) {
        return 0;
    }

    // `FBZZEngine.dll` は実行中ロックされ再ビルドできない。Engine ソースが古い DLL より
    // 新しければ、ここで一旦終了して cmake 再ビルド → 再起動を予約する (開発ビルドのみ)。
    if (canRelaunch && fbzz::core::CheckEngineFreshnessAndRelaunch()) {
        return 0;
    }

    // Editor-owned Play must keep its PID/job and never rebuild or relaunch itself.
    EditorPlayLog playLog(args.editorPlay ? args.playLog : std::filesystem::path{});
    if (args.editorPlay && !playLog.IsOpen()) {
        return 1;
    }
    if (args.editorPlay) FBZZ_LOG_INFO("Standalone Play process started: project=%s scene=%s",
        StringUtils::PathToUtf8(args.projectPath).c_str(), StringUtils::PathToUtf8(args.playScene).c_str());

    fbzz::ProjectResolver resolver;
    if (!resolver.Resolve(args.projectPath)) {
        // 無人起動でプロジェクト解決に失敗した場合は stderr / stdout の診断で終了する。
        if (batch || args.hidden || args.editorPlay) {
            FBZZ_LOG_ERROR("EditorLauncher: %ls", resolver.ErrorMessage().c_str());
            return batch ? 2 : 1;
        }
        MessageBoxW(nullptr, resolver.ErrorMessage().c_str(), L"FBZZ", MB_OK | MB_ICONERROR);
        return 1;
    }
    fbzz::LaunchProject project = resolver.Get();
    if (args.editorPlay) {
        std::error_code sceneError;
        if (!std::filesystem::is_regular_file(args.playScene, sceneError) || sceneError) {
            FBZZ_LOG_ERROR("Standalone Play selected scene is missing.");
            return 1;
        }
        project.sceneFile = args.playScene;
    }
    const std::filesystem::path executableDirectory = FileSystem::GetExecutableDirectory();
    const std::filesystem::path workingDirectory = FileSystem::Exists(executableDirectory / L".fbzz_proj")
        ? executableDirectory
        : project.root;
    // 配布物は exe 隣に Assets があるため exeDir を CWD にする。Editor から別プロジェクトを
    // `--project` 指定で Standalone 起動する場合は Assets が `project.root` にあるため、
    // 相対 shader path が解決できるよう CWD を切り替える。
    SetCurrentDirectoryW(workingDirectory.wstring().c_str());

    // 終了処理で落ちても残すため Uninstall しない。バッチは人が居ないので通知をダイアログにしない。
    /// @see Docs/design/crash-report.md
    const std::string crashAppName = args.standalone ? StringUtils::PathToUtf8(project.root.filename()) : "FBZZ Editor";
    core::CrashHandler::Install(workingDirectory, crashAppName);
    core::CrashHandler::NotifyUnreported(workingDirectory, crashAppName, !batch && !args.hidden && !args.editorPlay);
    core::DeveloperMode::InitFromCommandLine();

    auto& app = core::Application::Get();

    if (args.standalone) {
        // Standalone モードではウィンドウを正しいサイズで生成するため
        // `Application::Init()` の前に `ProjectSettings` を読み込む必要がある。
        // Init 後に `Resize()` するとウィンドウが一瞬デフォルトサイズで表示されてしまう。
        ProjectSettings settings;
        if (!settings.Load(StringUtils::PathToUtf8(project.settingsFile))) {
            if (args.editorPlay) {
                FBZZ_LOG_ERROR("Standalone Play could not load ProjectSettings.");
                return 1;
            }
            MessageBoxW(nullptr, L"ProjectSettings を読み込めませんでした。", L"FBZZ", MB_OK | MB_ICONERROR);
            return 1;
        }

        // Standalone は `ProjectSettings` の renderer 指定でレンダラーを生成する。
        // コマンドライン `--renderer=` があれば `Application::Init` 内でそちらが優先される。
        if (args.pixCapture.requested && settings.app.rendererBackend != renderer::RendererBackend::DX12) {
            FBZZ_LOG_ERROR("EditorLauncher: --pix-capture requires the DirectX 12 backend.");
            return 1;
        }
        if (!core::InitializePixCapture(args.pixCapture)) {
            return 1;
        }
        // Apply before window callbacks and script startup; the process policy also blocks later click reacquisition.
        core::Cursor::SetPhysicalInputAllowed(args.allowPhysicalInput);
        if (!app.Init(scene::MakeWindowConfig(settings), settings.app.rendererBackend)) {
            return 1;
        }

        auto& renderer = app.GetRenderer();
        auto& imguiRenderer = app.GetImGuiRenderer();
        renderer::ResourceManager resources(renderer);
        app.SetResourceManager(&resources);
        asset::AssetManager::Init(resources, StringUtils::PathToUtf8(project.root / L"Assets") + "/");

        bool started = true;
        if (args.editorPlay) {
            scene::StandaloneProjectModule game(renderer, resources, project.root, project.sceneFile, settings, true);
            app.Run(game);
            started = game.StartedSuccessfully();
            FBZZ_LOG_INFO("Standalone Play runtime finished (startup=%s).", started ? "ok" : "failed");
        } else {
            StandaloneApp standaloneApp(renderer, imguiRenderer, resources, project, settings);
            app.Run(standaloneApp);
        }
        // `ResourceManager` は app よりスコープが長いため `~ResourceManager()` が
        // `app::Shutdown()` より先に走る。事前に `Reset()` しないとデストラクタの
        // `LogLiveDebugResources` が GPU リソースを「外部保持」と誤判定して報告する。
        asset::AssetManager::UnloadAll();
        resources.Reset();
        if (args.editorPlay && !started) {
            FBZZ_LOG_ERROR("Standalone Play runtime initialization failed.");
            app.Shutdown();
            return 1;
        }
    } else {
        // Editor も起動時プロジェクトの renderer 設定に従う。レンダラーはプロジェクト
        // 読込前に生成するため設定をここで先読みしてバックエンドを渡す (`--renderer=`
        // があれば優先)。Load 失敗時は既定の DX12 で開く (本読込は `OpenProject` が行う)。
        ProjectSettings settings;
        settings.Load(StringUtils::PathToUtf8(project.settingsFile));
        if (args.pixCapture.requested && settings.app.rendererBackend != renderer::RendererBackend::DX12) {
            FBZZ_LOG_ERROR("EditorLauncher: --pix-capture requires the DirectX 12 backend.");
            return batch ? 2 : 1;
        }
        if (!core::InitializePixCapture(args.pixCapture)) return batch ? 2 : 1;
        // Hidden batch runs must not activate briefly before the later SW_HIDE.
        if (batch) {
            core::Cursor::SetPhysicalInputAllowed(false);
        }
        if (!app.Init(core::Window::Config{}, settings.app.rendererBackend)) {
            return 1;
        }

        auto& renderer    = app.GetRenderer();
        auto& imguiRenderer = app.GetImGuiRenderer();
        // バッチは window が occluded でも offscreen RT を描く。通常 Editor は DXGI の occlusion 休止を維持する。
        renderer.SetRenderWhenOccluded(batch);
        renderer::ResourceManager resources(renderer);
        app.SetResourceManager(&resources);

        const std::filesystem::path assetRoot = project.root / L"Assets";
        asset::AssetManager::Init(resources, StringUtils::PathToUtf8(assetRoot) + "/");

        // 隠すのは窓だけ。スワップチェーンと RT はそのまま作られるので、描画と撮影は通常起動と同じ経路を通る。
        if (args.hidden) {
            ShowWindow(app.GetWindow().GetHandle(), SW_HIDE);
        }

        editor::EditorApp editorApp;
        const int startupFailure = args.batchScenario.empty() ? 1 : 2;
        if (!editorApp.Init(renderer, imguiRenderer, resources, app.GetWindow(), args.enableViewports)) {
            return startupFailure;
        }
        if (!editorApp.OpenProject(StringUtils::PathToUtf8(project.root),
                                   StringUtils::PathToUtf8(project.settingsFile),
                                   StringUtils::PathToUtf8(project.sceneFile))) {
            return startupFailure;
        }
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
        // standalone 側と同様、`~ResourceManager()` に先んじて GPU リソースを解放する。
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
