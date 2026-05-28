// FBZZ Engine
// main.cpp | fbzz::editor_launcher
// Standalone project-aware editor executable
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/ProjectSettings.hpp>
#include <Engine/Renderer/DebugCamera.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/AnimatorDebugDrawSystem.hpp>
#include <Engine/Scene/Systems/AnimatorSystem.hpp>
#include <Engine/Scene/Systems/ConstraintDebugDrawSystem.hpp>
#include <Engine/Scene/Systems/PhysicsSystem.hpp>
#include <Engine/Scene/Systems/RenderSystem.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Editor/EditorApp.hpp>
#include <Physics/World.hpp>

#include <Windows.h>
#include <shellapi.h>
#include <toml++/toml.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace fbzz::editor_launcher {

namespace {

struct LaunchProject {
    std::filesystem::path root;
    std::filesystem::path projectFile;
    std::filesystem::path settingsFile;
    std::filesystem::path sceneFile;
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

std::filesystem::path FindProjectPathFromArgs()
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) return {};

    std::filesystem::path projectPath;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--project" && i + 1 < argc) {
            projectPath = argv[i + 1];
            break;
        }
    }

    LocalFree(argv);
    return projectPath;
}

std::filesystem::path ReadTomlRelativePath(const toml::table& table, const char* tableName, const char* key)
{
    const std::string value = table[tableName][key].value_or(std::string{});
    return value.empty() ? std::filesystem::path{} : std::filesystem::path(Utf8ToWide(value));
}

bool ResolveProject(LaunchProject& project, std::wstring& errorMessage)
{
    project.root = MakeAbsolute(FindProjectPathFromArgs());
    if (project.root.empty()) {
        errorMessage = L"Project path was not specified.\n\nFBZZEditor.exe --project <path>";
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
    const std::filesystem::path settingsPath = ReadTomlRelativePath(projectTable, "project", "settings_path");
    const std::filesystem::path defaultScene = ReadTomlRelativePath(projectTable, "project", "default_scene");
    if (settingsPath.empty()) {
        errorMessage = L".fbzz_proj does not define project.settings_path.";
        return false;
    }

    project.settingsFile = MakeAbsolute(project.root / settingsPath);
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
    if (scenePath.empty()) {
        scenePath = defaultScene;
    }
    if (scenePath.empty()) {
        errorMessage = L"Project does not define a start scene.";
        return false;
    }

    project.sceneFile = MakeAbsolute(project.root / scenePath);
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

void WarmupRenderResources(scene::Scene& scene,
                           renderer::IRenderer& renderer,
                           renderer::ResourceManager& resources,
                           editor::EditorApp& editorApp,
                           const renderer::Camera& sceneCamera,
                           const renderer::Camera& gameCamera,
                           fbzz::LayerMask gameCullingMask)
{
    // WHY: RenderSystem は初回呼び出しで shader / PSO / shadow map / GBuffer などを lazy initialize する。
    // その負荷を最初の可視フレームに乗せると、Release では起動直後だけ FPS 表示が大きく落ちる。
    // WHAT: メインループ開始前に viewport RT へ 1 回描画し、描画リソースを先に生成しておく。
    renderer.BeginFrame();

    const auto sceneRT = editorApp.GetViewportRT();
    if (sceneRT.IsValid()) {
        renderer.SetRenderTarget(sceneRT, resources);
        renderer.Clear({ 0.05f, 0.05f, 0.08f, 1.0f });
        auto sceneRenderSettings = editorApp.GetContext().projectSettings.render;
        scene::RenderSystem(scene, renderer, resources, sceneCamera, sceneRT, &sceneRenderSettings);
    }

    const auto gameRT = editorApp.GetGameViewportRT();
    if (gameRT.IsValid()) {
        renderer.SetRenderTarget(gameRT, resources);
        renderer.Clear({ 0.02f, 0.02f, 0.05f, 1.0f });
        scene::RenderSystem(scene,
                            renderer,
                            resources,
                            gameCamera,
                            gameRT,
                            &editorApp.GetContext().projectSettings.render,
                            gameCullingMask);
    }

    renderer.SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>{}, resources);
    renderer.Clear({ 0.02f, 0.02f, 0.02f, 1.0f });
    renderer.EndFrame();
}

} // namespace

int Run()
{
    LaunchProject project;
    std::wstring errorMessage;
    if (!ResolveProject(project, errorMessage)) {
        MessageBoxW(nullptr, errorMessage.c_str(), L"FBZZ Editor", MB_OK | MB_ICONERROR);
        return 1;
    }

    SetCurrentDirectoryW(GetExecutableDirectory().wstring().c_str());

    auto& app = core::Application::Get();
    if (!app.Init()) return 1;

    auto& renderer = app.GetRenderer();
    renderer::ResourceManager resources(renderer);

    const std::filesystem::path assetRoot = project.root / L"Assets";
    asset::AssetManager::Init(resources, PathToUtf8(assetRoot) + "/");

    editor::EditorApp editorApp;
    if (!editorApp.Init(renderer, resources, app.GetWindow())) {
        app.Shutdown();
        return 1;
    }

    auto scene = std::make_unique<scene::Scene>();
    editorApp.GetContext().activeScene = scene.get();
    if (!editorApp.OpenProject(PathToUtf8(project.root), PathToUtf8(project.settingsFile), PathToUtf8(project.sceneFile))) {
        editorApp.Shutdown();
        asset::AssetManager::UnloadAll();
        app.Shutdown();
        return 1;
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
    debugCamera.camera.m_aspect = 1920.0f / 1080.0f;
    editorApp.GetContext().editorCamera = &debugCamera.camera;

    {
        const auto sceneRT = editorApp.GetViewportRT();
        if (auto* rt = resources.Get(sceneRT)) {
            debugCamera.camera.m_aspect = static_cast<float>(rt->GetWidth()) / static_cast<float>(rt->GetHeight());
        }

        renderer::Camera gameCamera = debugCamera.camera;
        fbzz::LayerMask gameCullingMask = fbzz::Layer::Everything;
        const auto gameRT = editorApp.GetGameViewportRT();
        if (auto* rt = resources.Get(gameRT)) {
            gameCamera.m_aspect = static_cast<float>(rt->GetWidth()) / static_cast<float>(rt->GetHeight());
        }

        for (auto& go : scene->GameObjects()) {
            auto* cameraComponent = go.GetComponent<scene::CameraComponent>();
            if (!go.activeSelf() || !cameraComponent || !cameraComponent->enabled || !cameraComponent->isMain)
                continue;

            gameCamera.m_position = go.transform.position;
            gameCamera.m_rotation = go.transform.rotation;
            gameCamera.m_fovY     = cameraComponent->fovY;
            gameCamera.m_near     = cameraComponent->nearZ;
            gameCamera.m_far      = cameraComponent->farZ;
            gameCullingMask       = cameraComponent->cullingMask;
            break;
        }

        WarmupRenderResources(*scene, renderer, resources, editorApp, debugCamera.camera, gameCamera, gameCullingMask);

        // WHY: warmup にかかった時間を最初の DeltaTime / FPS 表示へ混ぜない。
        // WHAT: Time をここで初期化し、メインループの次フレームから通常計測を始める。
        core::Time::Tick();
    }

    while (app.IsRunning()) {
        core::Time::Tick();
        input::Input::Update();
        app.GetWindow().PollEvents();
        if (app.GetWindow().ShouldClose()) {
            app.Quit();
            break;
        }

        const float dt = core::Time::DeltaTime();
        editorApp.BeginFrame();

        auto* playMode = editorApp.GetContext().playMode;
        if (playMode->ApplyPendingRestore(*scene)) {
            ApplyPhysicsSettings(physicsWorld, editorApp.GetContext().projectSettings);
            physicsAccumulator = 0.0f;
        }

        if (!playMode->IsPlaying()) {
            debugCamera.Update(dt);
        }

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
                // smoothstep
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

            const int physicsHz = settings.physics.hz < 1 ? 1 : settings.physics.hz;
            const float fixedDt = 1.0f / static_cast<float>(physicsHz);

            if (stepFrame) {
                scene::PhysicsSystem(*scene, physicsWorld, fixedDt);
            } else {
                physicsAccumulator += dt;
                const float maxAccumulatedTime = fixedDt * 8.0f;
                if (physicsAccumulator > maxAccumulatedTime)
                    physicsAccumulator = maxAccumulatedTime;
                while (physicsAccumulator >= fixedDt) {
                    scene::PhysicsSystem(*scene, physicsWorld, fixedDt);
                    physicsAccumulator -= fixedDt;
                }
            }
            scene::TransformSystem(*scene);
        } else {
            physicsAccumulator = 0.0f;
        }
        scene::AnimatorSystem(*scene, resources, stepFrame ? (1.0f / 60.0f) : dt);

        const auto sceneRT = editorApp.GetViewportRT();
        const auto gameRT = editorApp.GetGameViewportRT();

        if (auto* rt = resources.Get(sceneRT)) {
            debugCamera.camera.m_aspect = static_cast<float>(rt->GetWidth()) / static_cast<float>(rt->GetHeight());
        }

        float gameAspect = debugCamera.camera.m_aspect;
        if (auto* rt = resources.Get(gameRT)) {
            gameAspect = static_cast<float>(rt->GetWidth()) / static_cast<float>(rt->GetHeight());
        }
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
        if (editorApp.GetContext().projectSettings.render.showColliders) {
            renderer::DebugDraw::BeginFrame(renderer, resources, debugCamera.camera.GetViewProjection());
            scene::ConstraintDebugDrawSystem(physicsWorld, renderer);
            renderer::DebugDraw::Flush();
        }
        if (editorApp.GetContext().showSkeleton) {
            scene::AnimatorDebugDrawSystem(*scene, renderer, resources, debugCamera.camera.GetViewProjection());
        }

        if (gameRT.IsValid()) {
            renderer.SetRenderTarget(gameRT, resources);
            renderer.Clear({ 0.02f, 0.02f, 0.05f, 1.0f });
            scene::RenderSystem(*scene,
                                renderer,
                                resources,
                                gameCamera,
                                gameRT,
                                &editorApp.GetContext().projectSettings.render,
                                gameCullingMask);
        }

        renderer.SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>{}, resources);
        renderer.Clear({ 0.02f, 0.02f, 0.02f, 1.0f });
        editorApp.GetContext().activeScene = scene.get();
        editorApp.RenderPanels(editorApp.GetContext());
        editorApp.EndFrame(renderer);

        renderer.EndFrame();
    }

    editorApp.Shutdown();
    asset::AssetManager::UnloadAll();
    app.Shutdown();
    return 0;
}

} // namespace fbzz::editor_launcher

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    return fbzz::editor_launcher::Run();
}
