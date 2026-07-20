// FBZZ Engine
// main.cpp | fbzz::vfx_editor_launcher
// 独立VFXEditorのWin32エントリポイント
#include <Editor/VFX/VFXEditorApp.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/ProjectResolver.hpp>
#include <Engine/ProjectSettings.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <Windows.h>
#include <shellapi.h>
#include <filesystem>
#include <string>

namespace fbzz::vfx_editor_launcher {

namespace {

struct LaunchArgs {
    std::filesystem::path projectPath;
    std::filesystem::path assetPath;
};

LaunchArgs ParseArgs()
{
    LaunchArgs args;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) return args;
    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index];
        if (argument == L"--project" && index + 1 < argc)
            args.projectPath = argv[++index];
        else if (argument == L"--asset" && index + 1 < argc)
            args.assetPath = argv[++index];
    }
    LocalFree(argv);
    return args;
}

} // namespace

int Run()
{
    const LaunchArgs args = ParseArgs();
    if (args.projectPath.empty()) {
        MessageBoxW(nullptr, L"--project <path> を指定してください。", L"FBZZ VFX Editor",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    ProjectResolver resolver;
    if (!resolver.Resolve(args.projectPath)) {
        MessageBoxW(nullptr, resolver.ErrorMessage().c_str(), L"FBZZ VFX Editor",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    const LaunchProject project = resolver.Get();
    SetCurrentDirectoryW(project.root.wstring().c_str());

    ProjectSettings settings;
    if (!settings.Load(util::StringUtils::PathToUtf8(project.settingsFile))) {
        MessageBoxW(nullptr, L"ProjectSettingsを読み込めませんでした。", L"FBZZ VFX Editor",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    core::Window::Config windowConfig{};
    windowConfig.title = L"FBZZ VFX Editor";
    windowConfig.width = 1920;
    windowConfig.height = 1080;

    auto& application = core::Application::Get();
    if (!application.Init(windowConfig, settings.app.rendererBackend)) return 1;

    auto& renderer = application.GetRenderer();
    auto& imguiRenderer = application.GetImGuiRenderer();
    renderer::ResourceManager resources(renderer);
    const std::filesystem::path assetRoot = project.root / L"Assets";
    asset::AssetManager::Init(resources, util::StringUtils::PathToUtf8(assetRoot) + "/");

    editor::VFXEditorApp vfxEditor;
    if (!vfxEditor.Init(renderer, imguiRenderer, resources, application.GetWindow(),
                        util::StringUtils::PathToUtf8(project.root),
                        util::StringUtils::PathToUtf8(project.settingsFile),
                        util::StringUtils::PathToUtf8(args.assetPath))) {
        asset::AssetManager::UnloadAll();
        resources.Reset();
        application.Shutdown();
        return 1;
    }

    application.Run(vfxEditor);
    asset::AssetManager::UnloadAll();
    resources.Reset();
    application.Shutdown();
    return 0;
}

} // namespace fbzz::vfx_editor_launcher

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    return fbzz::vfx_editor_launcher::Run();
}
