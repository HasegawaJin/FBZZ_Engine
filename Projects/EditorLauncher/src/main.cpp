// FBZZ Engine
// main.cpp | fbzz::editor_launcher
// エディタ / スタンドアロン両対応のエントリポイント
//
// WHAT: コマンドライン引数を解析し、エディタモードとスタンドアロンモードを切り替える。
//   FBZZEditor.exe --project <path>              → エディタ起動 (既存)
//   FBZZEditor.exe --project <path> --standalone → エディタ UI なし・ゲームのみ起動
//   FBZZEditor.exe (exe 隣に .fbzz_proj あり)     → 配布版として Standalone 起動
//   FBZZEditor.exe (引数なし / .fbzz_proj なし)   → 開発用テンプレートを Editor 起動
//
// WHY: 新しい実行ファイルを増やさずに同一バイナリで両モードを実現する。
//      配布時は exe をリネーム (FBZZGame.exe 等) してアセットと並べるだけでよい。
//
// WHY (Util の配置): Utf8ToWide / PathToUtf8 / Exists / ReadText 等の文字列・パス変換は
//      EditorLauncher と Sandbox の両方で必要なため Engine/Util に集約した。
//      ここでは Engine の API を直接呼ぶことで実装の重複を排除している。
#include "StandaloneApp.hpp"
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Core/Application.hpp>
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

// --project と --standalone フラグを格納する構造体。
// WHY: 引数解析結果を Run() へ渡すための軽量な値型として分離する。
struct LaunchArgs {
    std::filesystem::path projectPath;
    bool                  standalone = false;
};

std::filesystem::path FindDefaultEditorProjectPath()
{
    // WHY: build/release/Binaries/Release/FBZZEditor.exe を直接起動する開発導線では、
    //      exe 隣に .fbzz_proj が存在しない。配布物と区別し、標準テンプレートを Editor で開く。
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

// コマンドライン引数を解析して LaunchArgs を返す。
// WHY: 引数なし起動は exe 隣に .fbzz_proj があれば配布版 Standalone、
//      なければ標準テンプレートを Standalone として開く。
//      テンプレートの build_root は未解決だが StandaloneApp が exe 隣の
//      SandboxScripts.dll にフォールバックするためスクリプトが動作する。
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
        else if (arg == L"--standalone")
            args.standalone = true;
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

    return args;
}

} // namespace

int Run()
{
    const LaunchArgs args = ParseArgs();

    fbzz::ProjectResolver resolver;
    if (!resolver.Resolve(args.projectPath)) {
        MessageBoxW(nullptr, resolver.ErrorMessage().c_str(), L"FBZZ", MB_OK | MB_ICONERROR);
        return 1;
    }
    const fbzz::LaunchProject& project = resolver.Get();

    SetCurrentDirectoryW(FileSystem::GetExecutableDirectory().wstring().c_str());

    auto& app = core::Application::Get();

    if (args.standalone) {
        // WHY: Standalone モードではウィンドウを正しいサイズで生成するために
        //      Application::Init() の前に ProjectSettings を読み込む必要がある。
        //      Init 後に Resize() するとウィンドウが一瞬デフォルトサイズで表示されてしまう。
        ProjectSettings settings;
        if (!settings.Load(StringUtils::PathToUtf8(project.settingsFile))) {
            MessageBoxW(nullptr, L"ProjectSettings を読み込めませんでした。", L"FBZZ", MB_OK | MB_ICONERROR);
            return 1;
        }

        if (!app.Init(scene::MakeWindowConfig(settings))) return 1;

        auto& renderer = app.GetRenderer();
        auto& imguiRenderer = app.GetImGuiRenderer();
        renderer::ResourceManager resources(renderer);
        asset::AssetManager::Init(resources, StringUtils::PathToUtf8(project.root / L"Assets") + "/");

        StandaloneApp standaloneApp(renderer, imguiRenderer, resources, project, settings);
        app.Run(standaloneApp);
    } else {
        if (!app.Init()) return 1;

        auto& renderer    = app.GetRenderer();
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
    }

    asset::AssetManager::UnloadAll();
    app.Shutdown();
    return 0;
}

} // namespace fbzz::editor_launcher

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    return fbzz::editor_launcher::Run();
}
