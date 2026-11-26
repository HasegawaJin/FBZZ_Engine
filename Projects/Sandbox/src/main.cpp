/// @file    main.cpp
/// @brief   Sandbox エディタ / スタンドアロンの起動分岐。
/// @author  Hasegawa Jin
/// @date    2026-05-21

/// @note 起動モード: `Sandbox.exe --project <path>` はエディタ起動、`--standalone` 併用でゲームのみ起動、
/// @note exe 隣に `.fbzz_proj` があれば配布物として Standalone 起動、引数なしなら開発用テンプレートを Editor 起動。
/// @note main.cpp は起動順序だけを読み取れる入口にする。引数解析・プロジェクト解決・Editor/Standalone のループ本体は
/// @note 専用ファイルへ分割し、実行モードごとの依存関係と責務を分ける。
#ifndef FBZZ_STANDALONE_TARGET
#include <Editor/EditorApp.hpp>
#endif
#include "LaunchArgs.hpp"
#include <Engine/Asset/Registry/AssetSession.hpp>
#include <Engine/Project/ProjectResolver.hpp>
#include <Engine/Scene/Hierarchy/SceneUtils.hpp>
#include <Engine/Scene/Runtime/StandaloneProjectModule.hpp>
#include <Engine/Util/Files/FileSystem.hpp>
#include <Engine/Util/Text/StringUtils.hpp>
#include <Engine/Core/Platform/Application.hpp>
#include <Engine/Core/Diagnostics/CrashHandler.hpp>
#include <Engine/Core/Platform/DeveloperMode.hpp>
#include <Engine/Core/Platform/EngineRebuildBootstrap.hpp>
#include <Engine/Project/ProjectSettings.hpp>
#include <Graphics/Renderer/Resources/ResourceManager.hpp>

#include <Windows.h>

#include <cstdint>
#include <filesystem>

namespace fbzz::sandbox {
namespace {

using fbzz::util::StringUtils;
using fbzz::util::FileSystem;

/// @brief プロジェクトをゲームとして起動する。
/// @param[in,out] app ウィンドウと描画を初期化し、ゲームループを実行するアプリケーション。
/// @param[in] project プロジェクトのルート、設定、開始シーンのパス。
/// @return ゲームループ終了で 0、設定の読み込みやアプリケーションの初期化に失敗した場合は 1。
[[nodiscard]] int RunStandalone(core::Application& app, const LaunchProject& project)
{
    // ウィンドウとレンダラーを作る前にプロジェクトの起動設定を読み込む。
    ProjectSettings settings;
    if (!settings.Load(StringUtils::PathToUtf8(project.settingsFile))) {
        MessageBoxW(nullptr, L"Failed to load ProjectSettings.", L"FBZZ Sandbox", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Application::Init は明示された --renderer= をプロジェクトの設定より優先する。
    if (!app.Init(scene::MakeWindowConfig(settings), settings.app.rendererBackend)) {
        return 1;
    }

    // ゲームループの間、プロジェクトのアセットを参照できるようにする。
    auto& renderer = app.GetRenderer();
    renderer::ResourceManager resources(renderer);
    app.SetResourceManager(&resources);
    asset::AssetSession assets(resources, StringUtils::PathToUtf8(project.root / L"Assets") + "/");

    scene::StandaloneProjectModule module(
        renderer, resources, project.root, project.sceneFile, settings);
    app.Run(module);
    return 0;
}

/// @brief プロジェクトを EditorApp で開く。
/// @param[in,out] app ウィンドウと描画を初期化し、編集ループを実行するアプリケーション。
/// @param[in] project プロジェクトのルート、設定、開始シーンのパス。
/// @return 編集ループ終了で 0、編集モードを使えないか起動に失敗した場合は 1。
[[nodiscard]] int RunEditor(core::Application& app, const LaunchProject& project)
{
#ifdef FBZZ_STANDALONE_TARGET
    (void)app;
    (void)project;
    return 1;
#else
    // プロジェクトを開く前に、ウィンドウの作成に必要なレンダラー設定を読む。
    ProjectSettings settings;
    settings.Load(StringUtils::PathToUtf8(project.settingsFile));
    if (!app.Init(core::Window::Config{}, settings.app.rendererBackend)) {
        return 1;
    }

    // EditorApp が参照する描画資源とプロジェクトのアセットを準備する。
    auto& renderer = app.GetRenderer();
    auto& imguiRenderer = app.GetImGuiRenderer();
    renderer::ResourceManager resources(renderer);
    app.SetResourceManager(&resources);

    const std::filesystem::path assetRoot = project.root / L"Assets";
    asset::AssetSession assets(resources, StringUtils::PathToUtf8(assetRoot) + "/");

    // 編集画面を初期化し、プロジェクトを開けた場合にループへ入る。
    editor::EditorApp editorApp;
    if (!editorApp.Init(renderer, imguiRenderer, resources, app.GetWindow())) {
        return 1;
    }
    const bool openedProject = editorApp.OpenProject(StringUtils::PathToUtf8(project.root),
                                                     StringUtils::PathToUtf8(project.settingsFile),
                                                     StringUtils::PathToUtf8(project.sceneFile));
    if (!openedProject) {
        return 1;
    }

    app.Run(editorApp);
    return 0;
#endif
}

} // namespace

/// @brief コマンドラインからプロジェクトを決めて Sandbox を実行する。
/// @see Docs/design/crash-report.md
int Run()
{
    // 開発ビルドで DLL の再ビルドが必要なら、ファイルのロックを解くために終了する。
    if (fbzz::core::CheckEngineFreshnessAndRelaunch()) {
        return 0;
    }

    // 起動設定からプロジェクトを確定し、見つからなかった理由を表示する。
    const LaunchArgs args = LaunchArgs::Parse();

    if (args.projectPath.empty()) {
        MessageBoxW(nullptr,
                    L"Project path was not specified and the Sandbox default project was not found.\n\nSandbox.exe --project <path>",
                    L"FBZZ Sandbox", MB_OK | MB_ICONERROR);
        return 1;
    }

    ProjectResolver resolver;
    if (!resolver.Resolve(args.projectPath)) {
        MessageBoxW(nullptr, resolver.ErrorMessage().c_str(), L"FBZZ Sandbox", MB_OK | MB_ICONERROR);
        return 1;
    }
    const LaunchProject& project = resolver.Get();

    // 作業ディレクトリを実行ファイルの隣へ移し、クラッシュレポートはプロジェクトのルートに置く。
    SetCurrentDirectoryW(FileSystem::GetExecutableDirectory().wstring().c_str());

    const std::string crashAppName = StringUtils::PathToUtf8(project.root.filename());
    core::CrashHandler::Install(project.root, crashAppName);
    core::CrashHandler::NotifyUnreported(project.root, crashAppName, true);
    core::DeveloperMode::InitFromCommandLine();

    // 起動モードに対応したループを実行し、終了時にアプリケーションを解放する。
    auto& app = core::Application::Get();
#ifdef FBZZ_STANDALONE_TARGET
    const int result = RunStandalone(app, project);
#else
    const int result = args.standalone ? RunStandalone(app, project) : RunEditor(app, project);
#endif

    app.Shutdown();
    return result;
}

} // namespace fbzz::sandbox

int main()
{
    return fbzz::sandbox::Run();
}
