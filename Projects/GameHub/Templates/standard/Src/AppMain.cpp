// {{PROJECT_NAME}}
// AppMain.cpp | {{CPP_NAMESPACE}}
// Editor / standalone dual-mode entry point.
// To add game scripts, edit GameMain.cpp — do not modify this file.
//
// WHAT:
//   {{TARGET_NAME}}.exe --project <path>              -> editor mode
//   {{TARGET_NAME}}.exe --project <path> --standalone -> game-only mode
//   {{TARGET_NAME}}.exe (no args, .fbzz_proj next to exe) -> distribution standalone
//   {{TARGET_NAME}}.exe (no args, no .fbzz_proj)          -> dev mode: walk up to find project root
#include "{{TARGET_NAME}}/ProjectAPI.hpp"

#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/ProjectSettings.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Renderer/DebugCamera.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/Systems/AnimatorDebugDrawSystem.hpp>
#include <Engine/Scene/Systems/AnimatorSystem.hpp>
#include <Engine/Scene/Systems/ConstraintDebugDrawSystem.hpp>
#include <Engine/Scene/Systems/IKSystem.hpp>
#include <Engine/Scene/Systems/PhysicsSystem.hpp>
#include <Engine/Scene/Systems/RenderSystem.hpp>
#include <Engine/Scene/Systems/ScriptSystem.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/Systems/UISystem.hpp>
#include <Editor/EditorApp.hpp>
#include <Editor/Util/SceneSerializer.hpp>
#include <Physics/World.hpp>

#include <Windows.h>
#include <shellapi.h>
#include <toml++/toml.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace {{CPP_NAMESPACE}} {

namespace {

struct LaunchProject {
    std::filesystem::path root;
    std::filesystem::path projectFile;
    std::filesystem::path settingsFile;
    std::filesystem::path sceneFile;
};

struct LaunchArgs {
    std::filesystem::path projectPath;
    bool                  standalone = false;
};

std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 0) return {};
    std::wstring wide(static_cast<size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), size);
    return wide;
}

std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string utf8(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, utf8.data(), size, nullptr, nullptr);
    return utf8;
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    return WideToUtf8(path.wstring());
}

bool Exists(const std::filesystem::path& path)
{
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

std::filesystem::path MakeAbsolute(const std::filesystem::path& path)
{
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    return ec ? path : absolute.lexically_normal();
}

std::filesystem::path GetExecutableDirectory()
{
    wchar_t buffer[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(buffer).parent_path();
}

// 開発時: exe から親ディレクトリを辿って .fbzz_proj を探す。
// WHY: exe は Binaries/$<CONFIG>/ に出力されるため、プロジェクトルートは
//      exe の 2 段上にある。最大 6 段まで遡って探す。
std::filesystem::path FindDefaultProjectPath()
{
    std::filesystem::path dir = GetExecutableDirectory();
    for (int i = 0; i < 6 && !dir.empty(); ++i) {
        if (Exists(dir / L".fbzz_proj"))
            return dir;
        dir = dir.parent_path();
    }
    return {};
}

// WHY: 引数なし起動の挙動を 2 段階で決める。
//   1. exe 隣に .fbzz_proj がある → 配布版: Standalone モードで起動。
//   2. ない → 開発モード: FindDefaultProjectPath() でエディタ起動。
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
        const std::filesystem::path exeDir = GetExecutableDirectory();
        if (Exists(exeDir / L".fbzz_proj")) {
            args.projectPath = exeDir;
            args.standalone  = true;
        } else {
            args.projectPath = FindDefaultProjectPath();
        }
    }

    return args;
}

std::filesystem::path ReadTomlRelativePath(const toml::table& table, const char* tableName, const char* key)
{
    const std::string value = table[tableName][key].value_or(std::string{});
    return value.empty() ? std::filesystem::path{} : std::filesystem::path(Utf8ToWide(value));
}

bool IsTemplatePlaceholder(const std::filesystem::path& path)
{
    const std::wstring value = path.wstring();
    return value.size() >= 4 && value.rfind(L"{{", 0) == 0;
}

// 絶対パスが root 直下に存在しない場合 (プロジェクト移動後) に
// パスの末尾からサフィックスを順に試して root 相対でファイルを探す。
std::filesystem::path ResolvePathUnderRoot(const std::filesystem::path& filePath,
                                           const std::filesystem::path& root)
{
    if (!filePath.is_absolute())
        return MakeAbsolute(root / filePath);

    if (Exists(filePath))
        return filePath;

    std::filesystem::path suffix;
    std::filesystem::path p = filePath;
    while (p.has_relative_path()) {
        const auto component = p.filename();
        suffix = suffix.empty() ? component : (component / suffix);
        p = p.parent_path();
        const auto candidate = MakeAbsolute(root / suffix);
        if (Exists(candidate))
            return candidate;
    }
    return MakeAbsolute(root / filePath);
}

bool ResolveProject(LaunchProject& project, const std::filesystem::path& projectPath, std::wstring& errorMessage)
{
    project.root = MakeAbsolute(projectPath);
    if (project.root.empty()) {
        errorMessage = L"Project path was not specified and the default project was not found.\n\n{{TARGET_NAME}}.exe --project <path>";
        return false;
    }
    if (!Exists(project.root)) {
        errorMessage = L"Project folder was not found.\n\n" + project.root.wstring();
        return false;
    }

    project.projectFile = project.root / L".fbzz_proj";
    const std::string projectText = ReadText(project.projectFile);
    if (projectText.empty()) {
        errorMessage = L".fbzz_proj could not be read.\n\n" + project.projectFile.wstring();
        return false;
    }

    toml::parse_result projectResult = toml::parse(projectText);
    if (!projectResult) {
        errorMessage = L".fbzz_proj could not be parsed.\n\n" + project.projectFile.wstring();
        return false;
    }

    const toml::table& projectTable = projectResult.table();
    std::filesystem::path settingsPath = ReadTomlRelativePath(projectTable, "project", "settings_path");
    const std::filesystem::path defaultScene = ReadTomlRelativePath(projectTable, "project", "default_scene");
    if (IsTemplatePlaceholder(settingsPath))
        settingsPath = L"ProjectSettings/ProjectSettings.toml";
    if (settingsPath.empty()) {
        errorMessage = L".fbzz_proj does not define project.settings_path.";
        return false;
    }

    project.settingsFile = ResolvePathUnderRoot(settingsPath, project.root);
    if (!Exists(project.settingsFile)) {
        errorMessage = L"ProjectSettings file was not found.\n\n" + project.settingsFile.wstring();
        return false;
    }

    const std::string settingsText = ReadText(project.settingsFile);
    if (settingsText.empty()) {
        errorMessage = L"ProjectSettings file could not be read.\n\n" + project.settingsFile.wstring();
        return false;
    }

    toml::parse_result settingsResult = toml::parse(settingsText);
    if (!settingsResult) {
        errorMessage = L"ProjectSettings file could not be parsed.\n\n" + project.settingsFile.wstring();
        return false;
    }

    const toml::table& settingsTable = settingsResult.table();
    std::filesystem::path scenePath = ReadTomlRelativePath(settingsTable, "runtime", "start_scene");
    if (scenePath.empty())
        scenePath = defaultScene;
    if (scenePath.empty()) {
        errorMessage = L"Project does not define a start scene.";
        return false;
    }

    project.sceneFile = ResolvePathUnderRoot(scenePath, project.root);
    if (!Exists(project.sceneFile)) {
        errorMessage = L"Start scene file was not found.\n\n" + project.sceneFile.wstring();
        return false;
    }

    return true;
}

void ApplyPhysicsSettings(physics::World& world, const ProjectSettings& settings)
{
    world.SetGravity(settings.physics.gravity);
    world.SetSubsteps(settings.physics.substeps);
}

renderer::Camera ResolveGameCamera(scene::Scene& scene, float aspectRatio)
{
    for (auto& go : scene.GameObjects()) {
        auto* cam = go.GetComponent<scene::CameraComponent>();
        if (!go.activeSelf() || !cam || !cam->enabled || !cam->isMain) continue;

        renderer::Camera result;
        result.m_position = go.transform.position;
        result.m_rotation = go.transform.rotation;
        result.m_fovY     = cam->fovY;
        result.m_near     = cam->nearZ;
        result.m_far      = cam->farZ;
        result.m_aspect   = aspectRatio;
        return result;
    }
    renderer::Camera fallback;
    fallback.m_aspect = aspectRatio;
    return fallback;
}

// =============================================================================
// スタンドアロンゲームループ
// =============================================================================

void RunStandaloneLoop(renderer::IRenderer& renderer,
                       renderer::ResourceManager& resources,
                       const LaunchProject& project,
                       const ProjectSettings& settings)
{
    auto& app = core::Application::Get();

    auto scene = std::make_unique<scene::Scene>();
    if (!editor::SceneSerializer::Load(*scene, project.sceneFile.string())) {
        FBZZ_LOG_ERROR("{{TARGET_NAME}} Standalone: scene load failed: %s", project.sceneFile.string().c_str());
        return;
    }

    physics::World physicsWorld;
    ApplyPhysicsSettings(physicsWorld, settings);
    float physicsAccumulator = 0.0f;

    core::Time::Tick();

    while (app.IsRunning()) {
        core::Time::Tick();
        input::Input::Update();
        app.GetWindow().PollEvents();
        if (app.GetWindow().ShouldClose()) { app.Quit(); break; }

        const float dt = core::Time::DeltaTime();

        scene::ScriptSystem(*scene, dt);
        scene::TransformSystem(*scene);

        const int   physicsHz = settings.physics.hz < 1 ? 60 : settings.physics.hz;
        const float fixedDt   = 1.0f / static_cast<float>(physicsHz);
        physicsAccumulator += dt;
        const float maxAccum  = fixedDt * 8.0f;
        if (physicsAccumulator > maxAccum) physicsAccumulator = maxAccum;
        while (physicsAccumulator >= fixedDt) {
            scene::PhysicsSystem(*scene, physicsWorld, fixedDt);
            physicsAccumulator -= fixedDt;
        }
        scene::TransformSystem(*scene);
        scene::LateScriptSystem(*scene, dt);
        scene::AnimatorSystem(*scene, resources, dt);
        scene::IKSystem(*scene, resources, dt);

        renderer.BeginFrame();
        renderer.SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>{}, resources);
        renderer.Clear({ 0.02f, 0.02f, 0.05f, 1.0f });

        const uint32_t w = app.GetWindow().GetWidth();
        const uint32_t h = app.GetWindow().GetHeight();
        const float aspect = (h > 0) ? (static_cast<float>(w) / static_cast<float>(h)) : 1.0f;
        const renderer::Camera gameCamera = ResolveGameCamera(*scene, aspect);

        scene::RenderSystem(*scene, renderer, resources, gameCamera, {}, &settings.render);
        scene::UISystem(*scene, renderer, resources,
                        static_cast<float>(w), static_cast<float>(h),
                        {}, true, gameCamera.GetViewProjection());
        renderer.EndFrame();
    }
}

// =============================================================================
// エディタループ
// =============================================================================

void RunEditorLoop(renderer::IRenderer& renderer,
                   renderer::ResourceManager& resources,
                   const LaunchProject& project)
{
    auto& app = core::Application::Get();

    editor::EditorApp editorApp;
    if (!editorApp.Init(renderer, resources, app.GetWindow()))
        return;

    auto scene = std::make_unique<scene::Scene>();
    editorApp.GetContext().activeScene = scene.get();
    if (!editorApp.OpenProject(PathToUtf8(project.root),
                               PathToUtf8(project.settingsFile),
                               PathToUtf8(project.sceneFile))) {
        editorApp.Shutdown();
        return;
    }

    physics::World physicsWorld;
    ApplyPhysicsSettings(physicsWorld, editorApp.GetContext().projectSettings);
    float physicsAccumulator = 0.0f;

    constexpr float kFocusAnimDuration = 0.30f;
    struct FocusAnim {
        bool          active   = false;
        math::Vector3 startPos = {};
        math::Vector3 endPos   = {};
        math::Vector3 target   = {};
        float         t        = 0.0f;
    } focusAnim;

    renderer::DebugCamera debugCamera;
    debugCamera.camera.m_position = { 0.0f, 2.5f, -8.0f };
    debugCamera.camera.m_aspect   = 1920.0f / 1080.0f;
    editorApp.GetContext().editorCamera = &debugCamera.camera;

    while (app.IsRunning()) {
        core::Time::Tick();
        input::Input::Update();
        app.GetWindow().PollEvents();
        if (app.GetWindow().ShouldClose()) { app.Quit(); break; }

        const float dt = core::Time::DeltaTime();
        editorApp.BeginFrame();

        auto* playMode = editorApp.GetContext().playMode;
        if (playMode->ApplyPendingRestore(*scene)) {
            ApplyPhysicsSettings(physicsWorld, editorApp.GetContext().projectSettings);
            physicsAccumulator = 0.0f;
        }

        if (!playMode->IsPlaying())
            debugCamera.Update(dt);

        {
            auto& ctx = editorApp.GetContext();
            if (ctx.requestFocusOnSelected) {
                ctx.requestFocusOnSelected = false;
                const math::Vector3 target = ctx.focusTargetPosition;
                const math::Vector3 dir    = debugCamera.camera.m_position - target;
                const float dist           = dir.Length();
                constexpr float kFocusDist = 5.0f;
                const math::Vector3 camDir = (dist > 0.01f)
                    ? dir * (1.0f / dist)
                    : math::Vector3{ 0.0f, 0.5f, -1.0f }.Normalized();

                focusAnim.active   = true;
                focusAnim.startPos = debugCamera.camera.m_position;
                focusAnim.endPos   = target + camDir * kFocusDist;
                focusAnim.target   = target;
                focusAnim.t        = 0.0f;
            }

            if (focusAnim.active) {
                focusAnim.t += dt / kFocusAnimDuration;
                if (focusAnim.t >= 1.0f) {
                    focusAnim.t      = 1.0f;
                    focusAnim.active = false;
                }
                const float s = focusAnim.t * focusAnim.t * (3.0f - 2.0f * focusAnim.t);
                debugCamera.camera.m_position = focusAnim.startPos
                    + (focusAnim.endPos - focusAnim.startPos) * s;
                debugCamera.LookAt(focusAnim.target);
            }
        }

        scene::TransformSystem(*scene);
        const bool stepFrame = playMode->ConsumeStep();
        if (playMode->IsPlaying() || stepFrame) {
            const auto& settings = editorApp.GetContext().projectSettings;
            ApplyPhysicsSettings(physicsWorld, settings);
            scene::ScriptSystem(*scene, stepFrame ? (1.0f / 60.0f) : dt);
            scene::TransformSystem(*scene);

            const int physicsHz = settings.physics.hz < 1 ? 1 : settings.physics.hz;
            const float fixedDt = 1.0f / static_cast<float>(physicsHz);
            if (stepFrame) {
                scene::PhysicsSystem(*scene, physicsWorld, fixedDt);
            } else {
                physicsAccumulator += dt;
                const float maxAccumulated = fixedDt * 8.0f;
                if (physicsAccumulator > maxAccumulated) physicsAccumulator = maxAccumulated;
                while (physicsAccumulator >= fixedDt) {
                    scene::PhysicsSystem(*scene, physicsWorld, fixedDt);
                    physicsAccumulator -= fixedDt;
                }
            }
            scene::TransformSystem(*scene);
            scene::LateScriptSystem(*scene, stepFrame ? (1.0f / 60.0f) : dt);
        } else {
            physicsAccumulator = 0.0f;
        }
        scene::AnimatorSystem(*scene, resources, stepFrame ? (1.0f / 60.0f) : dt);
        scene::IKSystem(*scene, resources, stepFrame ? (1.0f / 60.0f) : dt);

        const auto sceneRT = editorApp.GetViewportRT();
        const auto gameRT  = editorApp.GetGameViewportRT();

        if (auto* rt = resources.Get(sceneRT))
            debugCamera.camera.m_aspect = static_cast<float>(rt->GetWidth()) / static_cast<float>(rt->GetHeight());

        float gameAspect = debugCamera.camera.m_aspect;
        if (auto* rt = resources.Get(gameRT))
            gameAspect = static_cast<float>(rt->GetWidth()) / static_cast<float>(rt->GetHeight());

        renderer::Camera gameCamera = debugCamera.camera;
        gameCamera.m_aspect = gameAspect;
        fbzz::LayerMask gameCullingMask = fbzz::Layer::Everything;
        for (auto& go : scene->GameObjects()) {
            auto* cameraComponent = go.GetComponent<scene::CameraComponent>();
            if (!go.activeSelf() || !cameraComponent || !cameraComponent->enabled || !cameraComponent->isMain)
                continue;
            gameCamera.m_position = go.transform.position;
            gameCamera.m_rotation = go.transform.rotation;
            gameCamera.m_fovY     = cameraComponent->fovY;
            gameCamera.m_aspect   = gameAspect;
            gameCamera.m_near     = cameraComponent->nearZ;
            gameCamera.m_far      = cameraComponent->farZ;
            gameCullingMask       = cameraComponent->cullingMask;
            break;
        }

        renderer.BeginFrame();

        renderer.SetRenderTarget(sceneRT, resources);
        renderer.Clear({ 0.05f, 0.05f, 0.08f, 1.0f });
        auto sceneRenderSettings = editorApp.GetContext().projectSettings.render;
        sceneRenderSettings.selectedObjects.clear();
        sceneRenderSettings.selectedObjects.reserve(editorApp.GetContext().selectedEntities.size());
        for (scene::EntityID id : editorApp.GetContext().selectedEntities)
            sceneRenderSettings.selectedObjects.push_back({ id.index, id.generation });
        scene::RenderSystem(*scene, renderer, resources, debugCamera.camera, sceneRT, &sceneRenderSettings);
        {
            float w = 1920.0f, h = 1080.0f;
            if (auto* rt = resources.Get(sceneRT)) { w = static_cast<float>(rt->GetWidth()); h = static_cast<float>(rt->GetHeight()); }
            scene::UISystem(*scene, renderer, resources, w, h, { 0.f, 0.f }, false,
                            debugCamera.camera.GetViewProjection());
        }
        if (editorApp.GetContext().projectSettings.render.showColliders) {
            renderer::DebugDraw::BeginFrame(renderer, resources, debugCamera.camera.GetViewProjection());
            scene::ConstraintDebugDrawSystem(physicsWorld, renderer);
            renderer::DebugDraw::Flush();
        }
        if (editorApp.GetContext().showSkeleton)
            scene::AnimatorDebugDrawSystem(*scene, renderer, resources, debugCamera.camera.GetViewProjection());

        if (gameRT.IsValid()) {
            renderer.SetRenderTarget(gameRT, resources);
            renderer.Clear({ 0.02f, 0.02f, 0.05f, 1.0f });
            auto gameRenderSettings = editorApp.GetContext().projectSettings.render;
            gameRenderSettings.wireframeMode         = false;
            gameRenderSettings.showSelectionOutline  = false;
            gameRenderSettings.selectedObjects.clear();
            scene::RenderSystem(*scene, renderer, resources, gameCamera, gameRT,
                                &gameRenderSettings, gameCullingMask);
            {
                float w = 1920.0f, h = 1080.0f;
                if (auto* rt = resources.Get(gameRT)) { w = static_cast<float>(rt->GetWidth()); h = static_cast<float>(rt->GetHeight()); }
                scene::UISystem(*scene, renderer, resources, w, h, { 0.f, 0.f }, false,
                                gameCamera.GetViewProjection());
            }
        }

        renderer.SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>{}, resources);
        renderer.Clear({ 0.02f, 0.02f, 0.02f, 1.0f });
        editorApp.GetContext().activeScene = scene.get();
        editorApp.RenderPanels(editorApp.GetContext());
        editorApp.EndFrame(renderer);

        renderer.EndFrame();
    }

    editorApp.Shutdown();
}

} // namespace

int Run()
{
    const LaunchArgs args = ParseArgs();

    if (args.projectPath.empty()) {
        MessageBoxW(nullptr,
                    L"Project path was not specified and the project was not found.\n\n{{TARGET_NAME}}.exe --project <path>",
                    L"{{PROJECT_NAME}}", MB_OK | MB_ICONERROR);
        return 1;
    }

    LaunchProject project;
    std::wstring errorMessage;
    if (!ResolveProject(project, args.projectPath, errorMessage)) {
        MessageBoxW(nullptr, errorMessage.c_str(), L"{{PROJECT_NAME}}", MB_OK | MB_ICONERROR);
        return 1;
    }

    SetCurrentDirectoryW(GetExecutableDirectory().wstring().c_str());

    // WHY: RegisterScripts() はエディタ・スタンドアロンどちらでも必要。
    //      SceneSerializer がシーンを復元するときに ScriptFactory を参照するため
    //      シーンロードより前に呼ぶ必要がある。
    RegisterScripts();

    auto& app = core::Application::Get();

    if (args.standalone) {
        ProjectSettings settings;
        if (!settings.Load(PathToUtf8(project.settingsFile))) {
            MessageBoxW(nullptr, L"Failed to load ProjectSettings.", L"{{PROJECT_NAME}}", MB_OK | MB_ICONERROR);
            return 1;
        }

        core::Window::Config windowConfig;
        windowConfig.title      = Utf8ToWide(settings.window.title);
        windowConfig.width      = static_cast<uint32_t>(settings.window.width);
        windowConfig.height     = static_cast<uint32_t>(settings.window.height);
        windowConfig.fullscreen = settings.window.fullscreen;
        if (!app.Init(windowConfig)) return 1;

        auto& renderer = app.GetRenderer();
        renderer::ResourceManager resources(renderer);
        asset::AssetManager::Init(resources, PathToUtf8(project.root / L"Assets") + "/");

        RunStandaloneLoop(renderer, resources, project, settings);
    } else {
        if (!app.Init()) return 1;

        auto& renderer = app.GetRenderer();
        renderer::ResourceManager resources(renderer);

        asset::AssetManager::Init(resources, PathToUtf8(project.root / L"Assets") + "/");

        RunEditorLoop(renderer, resources, project);
    }

    asset::AssetManager::UnloadAll();
    app.Shutdown();
    return 0;
}

} // namespace {{CPP_NAMESPACE}}

int main()
{
    return {{CPP_NAMESPACE}}::Run();
}
