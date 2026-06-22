// FBZZ Engine
// main.cpp | fbzz::sandbox
// Sandbox エディタ / スタンドアロンの起動分岐
//
// WHAT:
//   sandbox.exe --project <path>              -> エディタ起動
//   sandbox.exe --project <path> --standalone -> ゲームのみ起動
//   sandbox.exe (exe 隣に .fbzz_proj あり)     -> 配布物として Standalone 起動
//   sandbox.exe (引数なし)                    -> 開発用テンプレートを Editor 起動
//
// WHY: main.cpp は起動順序だけを読み取れる入口にする。
//      引数解析、プロジェクト解決、Editor / Standalone のループ本体は専用ファイルへ分割し、
//      実行モードごとの依存関係と責務を明確にする。
#ifndef FBZZ_STANDALONE_TARGET
#include <Editor/EditorApp.hpp>
#endif
#include "LaunchArgs.hpp"
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/ProjectResolver.hpp>
#include <Engine/Scene/SceneUtils.hpp>
#include <Engine/Scene/StandaloneProjectModule.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/ProjectSettings.hpp>
#include <Engine/Renderer/ResourceManager.hpp>

#include <Windows.h>

#include <cstdint>
#include <filesystem>

namespace fbzz::sandbox {
namespace {

using fbzz::util::StringUtils;
using fbzz::util::FileSystem;

/// Engine / Renderer / AssetManager 初期化後に StandaloneModule を起動する。
[[nodiscard]] int RunStandalone(core::Application& app, const LaunchProject& project)
{
    ProjectSettings settings;
    if (!settings.Load(StringUtils::PathToUtf8(project.settingsFile))) {
        MessageBoxW(nullptr, L"Failed to load ProjectSettings.", L"FBZZ Sandbox", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (!app.Init(scene::MakeWindowConfig(settings))) return 1;

    auto& renderer = app.GetRenderer();
    renderer::ResourceManager resources(renderer);
    asset::AssetManager::Init(resources, StringUtils::PathToUtf8(project.root / L"Assets") + "/");

    scene::StandaloneProjectModule module(
        renderer, resources, project.root, project.sceneFile, settings);
    app.Run(module);
    return 0;
}

/// 共通EditorAppを直接起動し、Sandbox固有のEditorループを持たない。
[[nodiscard]] int RunEditor(core::Application& app, const LaunchProject& project)
{
#ifdef FBZZ_STANDALONE_TARGET
    (void)app;
    (void)project;
    return 1;
#else
    if (!app.Init()) return 1;

    auto& renderer = app.GetRenderer();
    auto& imguiRenderer = app.GetImGuiRenderer();
    renderer::ResourceManager resources(renderer);

    const std::filesystem::path assetRoot = project.root / L"Assets";
    asset::AssetManager::Init(resources, StringUtils::PathToUtf8(assetRoot) + "/");

    editor::EditorApp editorApp;
    if (!editorApp.Init(renderer, imguiRenderer, resources, app.GetWindow())) return 1;
    if (!editorApp.OpenProject(StringUtils::PathToUtf8(project.root),
                               StringUtils::PathToUtf8(project.settingsFile),
                               StringUtils::PathToUtf8(project.sceneFile))) return 1;
    app.Run(editorApp);
    return 0;
#endif
}

} // namespace

int Run()
{
    const LaunchArgs args = LaunchArgs::Parse();

    if (args.projectPath.empty()) {
        MessageBoxW(nullptr,
                    L"Project path was not specified and the Sandbox default project was not found.\n\nsandbox.exe --project <path>",
                    L"FBZZ Sandbox", MB_OK | MB_ICONERROR);
        return 1;
    }

    ProjectResolver resolver;
    if (!resolver.Resolve(args.projectPath)) {
        MessageBoxW(nullptr, resolver.ErrorMessage().c_str(), L"FBZZ Sandbox", MB_OK | MB_ICONERROR);
        return 1;
    }
    const LaunchProject& project = resolver.Get();

    SetCurrentDirectoryW(FileSystem::GetExecutableDirectory().wstring().c_str());

    auto& app = core::Application::Get();
#ifdef FBZZ_STANDALONE_TARGET
    // WHY: 配布用 exe は --standalone の指定漏れや引数なし起動でも必ずゲーム本体として起動する。
    const int result = RunStandalone(app, project);
#else
    const int result = args.standalone ? RunStandalone(app, project) : RunEditor(app, project);
#endif

    asset::AssetManager::UnloadAll();
    app.Shutdown();
    return result;
}

} // namespace fbzz::sandbox

int main()
{
    return fbzz::sandbox::Run();
}
