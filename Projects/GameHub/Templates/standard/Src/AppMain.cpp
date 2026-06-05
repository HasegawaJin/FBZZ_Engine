// {{PROJECT_NAME}}
// AppMain.cpp | {{CPP_NAMESPACE}}
// エディタ / スタンドアロン両対応エントリポイント
//
// WHAT:
//   {{TARGET_NAME}}.exe --project <path>              -> エディタ起動
//   {{TARGET_NAME}}.exe --project <path> --standalone -> ゲームのみ起動
//   {{TARGET_NAME}}.exe (exe 隣に .fbzz_proj あり)     -> 配布物として Standalone 起動
//   {{TARGET_NAME}}.exe (引数なし)                    -> 開発モード: エディタ起動
//
// WHY: ゲーム固有のスクリプト登録は GameMain.cpp に委譲し、
//      AppMain.cpp は起動分岐・IModule 実装・ユーティリティに専念する。
//      Time / Input / Window / Memory / Profiler の共通フレーム処理は
//      Application::Run(IModule&) に集約されるため、各 Module は
//      「何を更新・描画するか」だけを実装すればよい。
#include "{{TARGET_NAME}}/ProjectAPI.hpp"

#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/IModule.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/ProjectSettings.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Renderer/DebugCamera.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/AnimatorDebugDrawSystem.hpp>
#include <Engine/Scene/Systems/AnimatorSystem.hpp>
#include <Engine/Scene/Systems/ConstraintDebugDrawSystem.hpp>
#include <Engine/Scene/Systems/IKSystem.hpp>
#include <Engine/Scene/Systems/PhysicsSystem.hpp>
#include <Engine/Scene/Systems/RenderSystem.hpp>
#include <Engine/Core/ILogSink.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/Systems/ScriptSystem.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/Systems/UISystem.hpp>
#ifndef FBZZ_STANDALONE_TARGET
#include <Editor/EditorApp.hpp>
#endif
#include <Physics/World.hpp>

#include <Windows.h>
#include <shellapi.h>
#include <toml++/toml.hpp>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace {{CPP_NAMESPACE}} {

namespace {

// ============================================================
// 起動補助型と関数
// ============================================================

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

// WHY: 引数なし起動の挙動を 2 段階で決める。
//   1. exe 隣に .fbzz_proj がある → 配布版: Standalone モードで起動。
//   2. ない → 開発モード: exe から最大 6 段親を遡って .fbzz_proj を探す。
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

void ApplyPhysicsSettings(fbzz::physics::World& world, const fbzz::ProjectSettings& settings)
{
    world.SetGravity(settings.physics.gravity);
    world.SetSubsteps(settings.physics.substeps);
}

void ApplyUISettings(const fbzz::ProjectSettings& settings)
{
    fbzz::scene::UISystemSetDefaultFontPath(settings.ui.defaultFontPath);
}

fbzz::renderer::Camera ResolveGameCamera(fbzz::scene::Scene& scene, float aspectRatio)
{
    for (auto& go : scene.GameObjects()) {
        auto* cam = go.GetComponent<fbzz::scene::CameraComponent>();
        if (!go.activeSelf() || !cam || !cam->enabled || !cam->isMain) continue;

        fbzz::renderer::Camera result;
        result.m_position = go.transform.position;
        result.m_rotation = go.transform.rotation;
        result.m_fovY     = cam->fovY;
        result.m_near     = cam->nearZ;
        result.m_far      = cam->farZ;
        result.m_aspect   = aspectRatio;
        return result;
    }
    fbzz::renderer::Camera fallback;
    fallback.m_aspect = aspectRatio;
    return fallback;
}

fbzz::core::Window::Config BuildWindowConfig(const fbzz::ProjectSettings& settings)
{
    fbzz::core::Window::Config windowConfig;
    windowConfig.title      = Utf8ToWide(settings.window.title);
    windowConfig.width      = static_cast<uint32_t>(settings.window.width);
    windowConfig.height     = static_cast<uint32_t>(settings.window.height);
    windowConfig.fullscreen = settings.window.fullscreen;
    return windowConfig;
}

// ============================================================
// StandaloneModule
// ============================================================

/// Application の共通ループからゲーム更新・描画を駆動する Module。
/// WHY: Time / Input / Window / Memory / Profiler は Application に集約し、ゲーム固有処理だけをここに分離する。
class StandaloneModule final : public fbzz::core::IModule {
public:
    StandaloneModule(fbzz::renderer::IRenderer& renderer,
                     fbzz::renderer::ResourceManager& resources,
                     const LaunchProject& project,
                     const fbzz::ProjectSettings& settings)
        : m_renderer(renderer)
        , m_resources(resources)
        , m_project(project)
        , m_settings(settings)
    {}

    [[nodiscard]] bool OnInit() override
    {
        m_scene = std::make_unique<fbzz::scene::Scene>();
        const std::string scenePathUtf8 = PathToUtf8(m_project.sceneFile);
        if (!fbzz::scene::SceneSerializer::LoadInPlace(*m_scene, scenePathUtf8, m_resources)) {
            FBZZ_LOG_ERROR("{{TARGET_NAME}} Standalone: シーンのロードに失敗しました: %s", scenePathUtf8.c_str());
            return false;
        }
        ApplyPhysicsSettings(m_physicsWorld, m_settings);
        ApplyUISettings(m_settings);
        m_physicsAccumulator = 0.0f;
        return true;
    }

    void OnUpdate(float dt) override
    {
        fbzz::scene::Script::SetPhysicsWorld(&m_physicsWorld);
        fbzz::scene::ScriptSystem(*m_scene, dt);
        fbzz::scene::TransformSystem(*m_scene);

        // WHAT: ProjectSettings の Hz に従って固定タイムステップ物理を複数回進める。
        // WHY: 描画 FPS が揺れても物理解の安定性を保つため、蓄積時間を最大 8 step に制限する。
        const int   physicsHz = m_settings.physics.hz < 1 ? 60 : m_settings.physics.hz;
        const float fixedDt   = 1.0f / static_cast<float>(physicsHz);
        m_physicsAccumulator += dt;
        const float maxAccum  = fixedDt * 8.0f;
        if (m_physicsAccumulator > maxAccum) m_physicsAccumulator = maxAccum;
        while (m_physicsAccumulator >= fixedDt) {
            fbzz::scene::PhysicsSystem(*m_scene, m_physicsWorld, fixedDt);
            m_physicsAccumulator -= fixedDt;
        }
        fbzz::scene::TransformSystem(*m_scene);
    }

    void OnLateUpdate(float dt) override
    {
        fbzz::scene::LateScriptSystem(*m_scene, dt);
        fbzz::scene::AnimatorSystem(*m_scene, m_resources, dt);
        fbzz::scene::IKSystem(*m_scene, m_physicsWorld, m_resources, dt);
    }

    void OnRender() override
    {
        auto& app = fbzz::core::Application::Get();
        m_renderer.BeginFrame();
        m_renderer.SetRenderTarget(fbzz::renderer::ResourceHandle<fbzz::renderer::RenderTargetTag>{}, m_resources);
        m_renderer.Clear({ 0.02f, 0.02f, 0.05f, 1.0f });

        const uint32_t w = app.GetWindow().GetWidth();
        const uint32_t h = app.GetWindow().GetHeight();
        const float aspect = (h > 0) ? (static_cast<float>(w) / static_cast<float>(h)) : 1.0f;
        const fbzz::renderer::Camera gameCamera = ResolveGameCamera(*m_scene, aspect);

        fbzz::scene::RenderSystemUIOptions uiOptions{};
        uiOptions.enabled            = true;
        uiOptions.viewportWidth      = static_cast<float>(w);
        uiOptions.viewportHeight     = static_cast<float>(h);
        uiOptions.mouseInCanvasSpace = fbzz::input::Input::MousePosition();
        uiOptions.mousePressed       = fbzz::input::Input::MouseButton(0);
        uiOptions.targetView         = fbzz::scene::UIRenderTargetView::GameViewport;
        fbzz::scene::RenderSystem(*m_scene, m_renderer, m_resources, gameCamera, {},
                                  &m_settings.render, fbzz::Layer::Everything, &uiOptions);
        m_renderer.EndFrame();
    }

    void OnShutdown() override
    {
        m_scene.reset();
    }

private:
    fbzz::renderer::IRenderer&          m_renderer;
    fbzz::renderer::ResourceManager&    m_resources;
    const LaunchProject&                m_project;
    const fbzz::ProjectSettings&        m_settings;
    std::unique_ptr<fbzz::scene::Scene> m_scene;
    fbzz::physics::World                m_physicsWorld;
    float                               m_physicsAccumulator = 0.0f;
};

// ============================================================
// EditorModule
// ============================================================

/// Application の共通ループから Editor UI・PlayMode・Scene / Game ビューポート描画を駆動する Module。
/// WHY: Editor 固有状態を Run() から切り離し、配布用 StandaloneModule と依存関係を分離しやすくする。
#ifndef FBZZ_STANDALONE_TARGET
class EditorModule final : public fbzz::core::IModule {
public:
    EditorModule(fbzz::renderer::IRenderer& renderer,
                 fbzz::renderer::ResourceManager& resources,
                 const LaunchProject& project)
        : m_renderer(renderer)
        , m_resources(resources)
        , m_project(project)
    {}

    [[nodiscard]] bool OnInit() override
    {
        auto& app = fbzz::core::Application::Get();
        if (!m_editorApp.Init(m_renderer, m_resources, app.GetWindow()))
            return false;

        m_scene = std::make_unique<fbzz::scene::Scene>();
        m_editorApp.GetContext().activeScene = m_scene.get();
        if (!m_editorApp.OpenProject(PathToUtf8(m_project.root),
                                     PathToUtf8(m_project.settingsFile),
                                     PathToUtf8(m_project.sceneFile)))
            return false;

        ApplyPhysicsSettings(m_physicsWorld, m_editorApp.GetContext().projectSettings);
        ApplyUISettings(m_editorApp.GetContext().projectSettings);
        m_physicsAccumulator = 0.0f;

        m_debugCamera.camera.m_position = { 0.0f, 2.5f, -8.0f };
        m_debugCamera.camera.m_aspect   = 1920.0f / 1080.0f;
        m_editorApp.GetContext().editorCamera = &m_debugCamera.camera;
        return true;
    }

    void OnUpdate(float dt) override
    {
        m_frameDt = dt;
        m_editorApp.BeginFrame();

        auto* playMode = m_editorApp.GetContext().playMode;
        if (playMode->ApplyPendingRestore(*m_scene)) {
            ApplyPhysicsSettings(m_physicsWorld, m_editorApp.GetContext().projectSettings);
            m_physicsAccumulator = 0.0f;
        }

        if (!playMode->IsPlaying())
            m_debugCamera.Update(dt);

        UpdateFocusAnimation(dt);

        fbzz::scene::TransformSystem(*m_scene);
        m_stepFrame = playMode->ConsumeStep();
        const float simulationDt = SimulationDeltaTime();
        if (playMode->IsPlaying() || m_stepFrame) {
            const auto& settings = m_editorApp.GetContext().projectSettings;
            ApplyPhysicsSettings(m_physicsWorld, settings);
            fbzz::scene::Script::SetPhysicsWorld(&m_physicsWorld);
            fbzz::scene::ScriptSystem(*m_scene, simulationDt);
            fbzz::scene::TransformSystem(*m_scene);

            const int physicsHz = settings.physics.hz < 1 ? 1 : settings.physics.hz;
            const float fixedDt = 1.0f / static_cast<float>(physicsHz);
            if (m_stepFrame) {
                fbzz::scene::PhysicsSystem(*m_scene, m_physicsWorld, fixedDt);
            } else {
                m_physicsAccumulator += dt;
                const float maxAccum = fixedDt * 8.0f;
                if (m_physicsAccumulator > maxAccum) m_physicsAccumulator = maxAccum;
                while (m_physicsAccumulator >= fixedDt) {
                    fbzz::scene::PhysicsSystem(*m_scene, m_physicsWorld, fixedDt);
                    m_physicsAccumulator -= fixedDt;
                }
            }
            fbzz::scene::TransformSystem(*m_scene);
            fbzz::scene::LateScriptSystem(*m_scene, simulationDt);
        } else {
            m_physicsAccumulator = 0.0f;
        }
    }

    void OnLateUpdate(float) override
    {
        const float simulationDt = SimulationDeltaTime();
        fbzz::scene::AnimatorSystem(*m_scene, m_resources, simulationDt);
        fbzz::scene::IKSystem(*m_scene, m_physicsWorld, m_resources, simulationDt);
    }

    void OnRender() override
    {
        const auto sceneRT = m_editorApp.GetViewportRT();
        const auto gameRT  = m_editorApp.GetGameViewportRT();

        if (auto* rt = m_resources.Get(sceneRT))
            m_debugCamera.camera.m_aspect = static_cast<float>(rt->GetWidth()) / static_cast<float>(rt->GetHeight());

        float gameAspect = m_debugCamera.camera.m_aspect;
        if (auto* rt = m_resources.Get(gameRT))
            gameAspect = static_cast<float>(rt->GetWidth()) / static_cast<float>(rt->GetHeight());

        const fbzz::renderer::Camera gameCamera = ResolveEditorGameCamera(gameAspect);
        const fbzz::LayerMask cullingMask       = ResolveGameCullingMask();

        m_renderer.BeginFrame();
        RenderSceneViewport(sceneRT);
        RenderGameViewport(gameRT, gameCamera, cullingMask);
        RenderEditorPanels();
        m_renderer.EndFrame();
    }

    void OnShutdown() override
    {
        m_editorApp.Shutdown();
        m_scene.reset();
    }

private:
    struct FocusAnim {
        bool              active   = false;
        fbzz::math::Vector3 startPos = {};
        fbzz::math::Vector3 endPos   = {};
        fbzz::math::Vector3 target   = {};
        float             t        = 0.0f;
    };

    [[nodiscard]] float SimulationDeltaTime() const
    {
        return m_stepFrame ? (1.0f / 60.0f) : m_frameDt;
    }

    void UpdateFocusAnimation(float dt)
    {
        auto& ctx = m_editorApp.GetContext();
        if (ctx.requestFocusOnSelected) {
            ctx.requestFocusOnSelected = false;
            const fbzz::math::Vector3 target = ctx.focusTargetPosition;
            const fbzz::math::Vector3 dir    = m_debugCamera.camera.m_position - target;
            const float dist                 = dir.Length();
            constexpr float FOCUS_DISTANCE = 5.0f;
            const fbzz::math::Vector3 camDir = (dist > 0.01f)
                ? dir * (1.0f / dist)
                : fbzz::math::Vector3{ 0.0f, 0.5f, -1.0f }.Normalized();

            m_focusAnim.active   = true;
            m_focusAnim.startPos = m_debugCamera.camera.m_position;
            m_focusAnim.endPos   = target + camDir * FOCUS_DISTANCE;
            m_focusAnim.target   = target;
            m_focusAnim.t        = 0.0f;
        }

        if (!m_focusAnim.active) return;

        constexpr float FOCUS_ANIM_DURATION = 0.30f;
        m_focusAnim.t += dt / FOCUS_ANIM_DURATION;
        if (m_focusAnim.t >= 1.0f) {
            m_focusAnim.t      = 1.0f;
            m_focusAnim.active = false;
        }

        // WHAT: smoothstep による補間で、選択オブジェクトへのフォーカス移動を急停止させない。
        const float s = m_focusAnim.t * m_focusAnim.t * (3.0f - 2.0f * m_focusAnim.t);
        m_debugCamera.camera.m_position = m_focusAnim.startPos
            + (m_focusAnim.endPos - m_focusAnim.startPos) * s;
        m_debugCamera.LookAt(m_focusAnim.target);
    }

    [[nodiscard]] fbzz::renderer::Camera ResolveEditorGameCamera(float gameAspect)
    {
        fbzz::renderer::Camera gameCamera = m_debugCamera.camera;
        gameCamera.m_aspect = gameAspect;

        for (auto& go : m_scene->GameObjects()) {
            auto* cam = go.GetComponent<fbzz::scene::CameraComponent>();
            if (!go.activeSelf() || !cam || !cam->enabled || !cam->isMain) continue;

            gameCamera.m_position = go.transform.position;
            gameCamera.m_rotation = go.transform.rotation;
            gameCamera.m_fovY     = cam->fovY;
            gameCamera.m_aspect   = gameAspect;
            gameCamera.m_near     = cam->nearZ;
            gameCamera.m_far      = cam->farZ;
            break;
        }
        return gameCamera;
    }

    [[nodiscard]] fbzz::LayerMask ResolveGameCullingMask()
    {
        for (auto& go : m_scene->GameObjects()) {
            auto* cam = go.GetComponent<fbzz::scene::CameraComponent>();
            if (!go.activeSelf() || !cam || !cam->enabled || !cam->isMain) continue;
            return cam->cullingMask;
        }
        return fbzz::Layer::Everything;
    }

    void RenderSceneViewport(fbzz::renderer::ResourceHandle<fbzz::renderer::RenderTargetTag> sceneRT)
    {
        m_renderer.SetRenderTarget(sceneRT, m_resources);
        m_renderer.Clear({ 0.05f, 0.05f, 0.08f, 1.0f });

        auto sceneRenderSettings = m_editorApp.GetContext().projectSettings.render;
        sceneRenderSettings.selectedObjects.clear();
        sceneRenderSettings.selectedObjects.reserve(m_editorApp.GetContext().selectedEntities.size());
        for (fbzz::scene::EntityID id : m_editorApp.GetContext().selectedEntities)
            sceneRenderSettings.selectedObjects.push_back({ id.index, id.generation });

        float w = 1920.0f, h = 1080.0f;
        if (auto* rt = m_resources.Get(sceneRT)) {
            w = static_cast<float>(rt->GetWidth());
            h = static_cast<float>(rt->GetHeight());
        }

        fbzz::scene::RenderSystemUIOptions uiOptions{};
        uiOptions.enabled        = true;
        uiOptions.viewportWidth  = w;
        uiOptions.viewportHeight = h;
        uiOptions.targetView     = fbzz::scene::UIRenderTargetView::SceneViewport;
        fbzz::scene::RenderSystem(*m_scene, m_renderer, m_resources,
                                  m_debugCamera.camera, sceneRT, &sceneRenderSettings,
                                  fbzz::Layer::Everything, &uiOptions);

        if (m_editorApp.GetContext().projectSettings.render.showColliders) {
            fbzz::renderer::DebugDraw::BeginFrame(m_renderer, m_resources,
                                                   m_debugCamera.camera.GetViewProjection());
            fbzz::scene::ConstraintDebugDrawSystem(m_physicsWorld, m_renderer);
            fbzz::renderer::DebugDraw::Flush();
        }
        if (m_editorApp.GetContext().showSkeleton) {
            fbzz::scene::AnimatorDebugDrawSystem(*m_scene, m_renderer, m_resources,
                                                  m_debugCamera.camera.GetViewProjection());
        }
    }

    void RenderGameViewport(fbzz::renderer::ResourceHandle<fbzz::renderer::RenderTargetTag> gameRT,
                            const fbzz::renderer::Camera& gameCamera,
                            fbzz::LayerMask gameCullingMask)
    {
        if (!gameRT.IsValid()) return;

        m_renderer.SetRenderTarget(gameRT, m_resources);
        m_renderer.Clear({ 0.02f, 0.02f, 0.05f, 1.0f });

        auto gameRenderSettings = m_editorApp.GetContext().projectSettings.render;
        gameRenderSettings.viewMode             = fbzz::renderer::ViewMode::Lit;
        gameRenderSettings.showSelectionOutline = false;
        gameRenderSettings.selectedObjects.clear();

        float w = 1920.0f, h = 1080.0f;
        if (auto* rt = m_resources.Get(gameRT)) {
            w = static_cast<float>(rt->GetWidth());
            h = static_cast<float>(rt->GetHeight());
        }

        fbzz::scene::RenderSystemUIOptions uiOptions{};
        uiOptions.enabled        = true;
        uiOptions.viewportWidth  = w;
        uiOptions.viewportHeight = h;
        uiOptions.targetView     = fbzz::scene::UIRenderTargetView::GameViewport;
        fbzz::scene::RenderSystem(*m_scene, m_renderer, m_resources,
                                  gameCamera, gameRT, &gameRenderSettings,
                                  gameCullingMask, &uiOptions);
    }

    void RenderEditorPanels()
    {
        m_renderer.SetRenderTarget(
            fbzz::renderer::ResourceHandle<fbzz::renderer::RenderTargetTag>{}, m_resources);
        m_renderer.Clear({ 0.02f, 0.02f, 0.02f, 1.0f });
        m_editorApp.GetContext().activeScene = m_scene.get();
        m_editorApp.RenderPanels(m_editorApp.GetContext());
        m_editorApp.EndFrame(m_renderer);
    }

    fbzz::renderer::IRenderer&          m_renderer;
    fbzz::renderer::ResourceManager&    m_resources;
    const LaunchProject&                m_project;
    fbzz::editor::EditorApp             m_editorApp;
    std::unique_ptr<fbzz::scene::Scene> m_scene;
    fbzz::physics::World                m_physicsWorld;
    fbzz::renderer::DebugCamera         m_debugCamera;
    FocusAnim                           m_focusAnim;
    float                               m_physicsAccumulator = 0.0f;
    float                               m_frameDt            = 0.0f;
    bool                                m_stepFrame          = false;
};
#endif

// ============================================================
// 起動関数
// ============================================================

/// ProjectSettings からウィンドウ設定を構築して StandaloneModule を起動する。
/// WHY: Application::Init() 前にウィンドウサイズを決定し、Renderer 初期化時点で正しいバックバッファを作る。
// ゲームログを exe 隣の game.log に書き出すシンク。
// WHY: WIN32 サブシステムはコンソールがなく printf が見えない。
//      OutputDebugString はデバッガなしでは確認できないため、ファイルに残す。
struct FileLogSink final : fbzz::core::ILogSink {
    std::ofstream file;
    void OnLog(const fbzz::core::LogEntry& entry) override {
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

[[nodiscard]] int RunStandalone(fbzz::core::Application& app, const LaunchProject& project)
{
    fbzz::ProjectSettings settings;
    if (!settings.Load(PathToUtf8(project.settingsFile))) {
        MessageBoxW(nullptr, L"Failed to load ProjectSettings.", L"{{PROJECT_NAME}}", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (!app.Init(BuildWindowConfig(settings))) return 1;

    auto& renderer = app.GetRenderer();
    fbzz::renderer::ResourceManager resources(renderer);
    fbzz::asset::AssetManager::Init(resources, PathToUtf8(project.root / L"Assets") + "/");

    StandaloneModule module(renderer, resources, project, settings);
    app.Run(module);
    return 0;
}

/// EditorModule を起動する。
[[nodiscard]] int RunEditor(fbzz::core::Application& app, const LaunchProject& project)
{
#ifdef FBZZ_STANDALONE_TARGET
    (void)app;
    (void)project;
    return 1;
#else
    if (!app.Init()) return 1;

    auto& renderer = app.GetRenderer();
    fbzz::renderer::ResourceManager resources(renderer);
    fbzz::asset::AssetManager::Init(resources, PathToUtf8(project.root / L"Assets") + "/");

    EditorModule module(renderer, resources, project);
    app.Run(module);
    return 0;
#endif
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

    // game.log を exe 隣に生成する (Standalone 時のみ。Editor では ConsolePanel を使う)
    // WHY: WIN32 サブシステムはコンソールがなく、デバッガなしでは OutputDebugString も見えない。
    //      INFO/WARN/ERROR は Release でも出力されるため、ファイルに残すことで問題を診断できる。
    FileLogSink logSink;
#ifdef FBZZ_STANDALONE_TARGET
    const bool isStandalone = true;
#else
    const bool isStandalone = args.standalone;
#endif
    if (isStandalone) {
        const std::filesystem::path logPath = GetExecutableDirectory() / L"game.log";
        logSink.file.open(logPath, std::ios::out | std::ios::trunc);
        if (logSink.file.is_open()) {
            const auto now = std::chrono::system_clock::now();
            const std::time_t t = std::chrono::system_clock::to_time_t(now);
            char timeBuf[64] = {};
            ctime_s(timeBuf, sizeof(timeBuf), &t);
            logSink.file << "=== {{PROJECT_NAME}} Log === " << timeBuf;
            fbzz::core::Logger::AddSink(&logSink);
        }
    }

    // WHY: RegisterScripts() はエディタ・スタンドアロンどちらでも必要。
    //      SceneSerializer がシーンを復元するときに ScriptFactory を参照するため、
    //      シーンロードより前に呼ぶ必要がある。
    RegisterScripts();
    FBZZ_LOG_INFO("{{PROJECT_NAME}}: RegisterScripts complete (%d types registered)",
        static_cast<int>(fbzz::scene::ScriptFactory::RegisteredTypeNames().size()));

    auto& app = fbzz::core::Application::Get();
#ifdef FBZZ_STANDALONE_TARGET
    // WHY: 配布用 exe は Editor をリンクしないため、起動引数に関係なく Standalone として実行する。
    const int result = RunStandalone(app, project);
#else
    const int result = args.standalone ? RunStandalone(app, project) : RunEditor(app, project);
#endif

    fbzz::asset::AssetManager::UnloadAll();
    app.Shutdown();
    fbzz::core::Logger::RemoveSink(&logSink);
    return result;
}

} // namespace {{CPP_NAMESPACE}}

int main()
{
    return {{CPP_NAMESPACE}}::Run();
}
