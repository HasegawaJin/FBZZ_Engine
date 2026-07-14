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
#include <Engine/Core/EngineRebuildBootstrap.hpp>
#include <Engine/Core/IModule.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/ProjectSettings.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Renderer/DebugCamera.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneUtils.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/StandaloneProjectModule.hpp>
#include <Engine/Scene/Systems/DebugDrawSystem.hpp>
#include <Engine/Core/ILogSink.hpp>
#include <Engine/Scene/Systems/RenderSystem.hpp>
#include <Engine/Scene/SceneManager.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/Systems/UISystem.hpp>
#include <Engine/Profiler/ProfileScope.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#ifndef FBZZ_STANDALONE_TARGET
#include <Editor/EditorApp.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <imgui.h>
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
    return fbzz::util::StringUtils::ToWide(text);
}

std::string WideToUtf8(const std::wstring& text)
{
    return fbzz::util::StringUtils::ToNarrow(text);
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    return fbzz::util::FileSystem::PathToUtf8(path);
}

bool Exists(const std::filesystem::path& path)
{
    return fbzz::util::FileSystem::Exists(path);
}

std::string ReadText(const std::filesystem::path& path)
{
    std::string text;
    fbzz::util::FileSystem::ReadText(path, text);
    return text;
}

std::filesystem::path MakeAbsolute(const std::filesystem::path& path)
{
    return fbzz::util::FileSystem::MakeAbsolute(path);
}

std::filesystem::path GetExecutableDirectory()
{
    return fbzz::util::FileSystem::GetExecutableDirectory();
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
    return value.empty() ? std::filesystem::path{} : fbzz::util::FileSystem::PathFromUtf8(value);
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

// ============================================================
// StandaloneModule
// ============================================================

/// Application の共通ループからゲーム更新・描画を駆動する Module。
/// WHY: Time / Input / Window / Memory / Profiler は Application に集約し、ゲーム固有処理だけをここに分離する。
// StandaloneのゲームループはEngine側のStandaloneProjectModuleを使用する。
// WHY: Sandboxと生成プロジェクトの実行経路を一致させ、修正漏れを防ぐ。

// ============================================================
// EditorModule
// ============================================================

/// Application の共通ループから Editor UI・PlayMode・Scene / Game ビューポート描画を駆動する Module。
/// WHY: Editor 固有状態を Run() から切り離し、配布用 StandaloneModule と依存関係を分離しやすくする。
#ifndef FBZZ_STANDALONE_TARGET
class EditorModule final : public fbzz::core::IModule {
public:
    EditorModule(fbzz::renderer::IRenderer& renderer,
                 fbzz::renderer::IImGuiRenderer& imguiRenderer,
                 fbzz::renderer::ResourceManager& resources,
                 const LaunchProject& project)
        : m_renderer(renderer)
        , m_imguiRenderer(imguiRenderer)
        , m_resources(resources)
        , m_project(project)
    {}

    [[nodiscard]] bool OnInit() override
    {
        auto& app = fbzz::core::Application::Get();
        if (!m_editorApp.Init(m_renderer, m_imguiRenderer, m_resources, app.GetWindow()))
            return false;

        m_scene = std::make_unique<fbzz::scene::Scene>();
        m_editorApp.GetContext().activeScene = m_scene.get();
        if (!m_editorApp.OpenProject(PathToUtf8(m_project.root),
                                     PathToUtf8(m_project.settingsFile),
                                     PathToUtf8(m_project.sceneFile)))
            return false;

        fbzz::scene::ApplyPhysicsSettings(m_physicsWorld, m_editorApp.GetContext().projectSettings);
        fbzz::scene::ApplyUISettings(m_editorApp.GetContext().projectSettings, &m_gameUICtx);
        fbzz::scene::ApplyUISettings(m_editorApp.GetContext().projectSettings, &m_sceneUICtx);
        auto& sceneManager = m_editorApp.GetSceneManager();
        sceneManager.SetScene(m_scene.get());
        sceneManager.SetPhysicsHz(m_editorApp.GetContext().projectSettings.physics.hz);

        // Play 中の LoadScene が解決できるよう、プロジェクト内の全 Scene を名前で登録する。
        const std::filesystem::path scenesDir = m_project.root / L"Assets" / L"Scenes";
        std::error_code fsErr;
        if (std::filesystem::exists(scenesDir, fsErr)) {
            std::filesystem::recursive_directory_iterator sceneIt(
                scenesDir,
                std::filesystem::directory_options::skip_permission_denied,
                fsErr);
            const std::filesystem::recursive_directory_iterator sceneEnd;
            while (sceneIt != sceneEnd && !fsErr) {
                if (sceneIt->is_regular_file(fsErr) && sceneIt->path().extension() == L".scene") {
                    sceneManager.RegisterFromFile(
                        PathToUtf8(sceneIt->path().stem()),
                        PathToUtf8(sceneIt->path()),
                        m_resources);
                }
                sceneIt.increment(fsErr);
            }
        }

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
        auto& sceneManager = m_editorApp.GetSceneManager();
        if (playMode->ApplyPendingRestore(*m_scene)) {
            // WHY: Play 中にシーン遷移していた場合、SceneManager が別の scene を active にしている。
            //      Stop 時は必ず編集用 m_scene に戻す。
            sceneManager.SetScene(m_scene.get());
            m_editorApp.GetContext().activeScene = m_scene.get();
            // WHY: World は物理同期とは別に m_contactCache / m_prevEvents を保持する。
            //      前 Play セッションの Collider* が残ったまま次 Play が始まると物理が誤動作するため、
            //      Stop 復元のタイミングで World を丸ごとリセットする。
            m_physicsWorld = fbzz::physics::World{};
            fbzz::scene::ApplyPhysicsSettings(m_physicsWorld, m_editorApp.GetContext().projectSettings);
            sceneManager.SetSimulating(false);
        }

        if (!playMode->IsPlaying())
            m_debugCamera.Update(dt);

        UpdateFocusAnimation(dt);

        m_stepFrame = playMode->ConsumeStep();
        const float simulationDt = SimulationDeltaTime();
        const auto& settings = m_editorApp.GetContext().projectSettings;
        fbzz::scene::Script::SetPhysicsWorld(&m_physicsWorld);
        fbzz::scene::ApplyPhysicsSettings(m_physicsWorld, settings);
        sceneManager.SetSimulating(playMode->IsPlaying() || m_stepFrame);
        sceneManager.SetPhysicsHz(settings.physics.hz);
        sceneManager.SetSingleStep(m_stepFrame);
        sceneManager.Update(simulationDt, m_physicsWorld);
        m_editorApp.GetContext().activeScene = sceneManager.GetActive();
    }

    void OnLateUpdate(float) override
    {
        m_editorApp.GetSceneManager().LateUpdate(SimulationDeltaTime(), m_physicsWorld);
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

        fbzz::scene::Scene* activeScene = m_editorApp.GetSceneManager().GetActive();
        const fbzz::renderer::Camera gameCamera = activeScene
            ? fbzz::scene::ResolveEditorGameCamera(*activeScene, m_debugCamera.camera, gameAspect)
            : m_debugCamera.camera;
        const fbzz::LayerMask cullingMask = activeScene
            ? fbzz::scene::ResolveGameCullingMask(*activeScene)
            : fbzz::Layer::Everything;

        { FBZZ_PROFILE_SCOPE("Renderer::BeginFrame"); m_renderer.BeginFrame(); }
        RenderSceneViewport(sceneRT);
        RenderGameViewport(gameRT, gameCamera, cullingMask);
        RenderEditorPanels();
        { FBZZ_PROFILE_SCOPE("Renderer::EndFrame"); m_renderer.EndFrame(); }
    }

    void OnShutdown() override
    {
        // WHY: ScriptSystem の OnDestroy / destructor を FreeLibrary より前に実行するため、
        //      全 Scene を Clear() してから Shutdown() する。順序を守らないと DLL アンロード後に
        //      vtable を踏んでクラッシュする。
        auto& sceneManager = m_editorApp.GetSceneManager();
        if (fbzz::scene::Scene* s = sceneManager.GetActive(); s && s != m_scene.get())
            s->Clear();
        m_scene->Clear();
        sceneManager.SetScene(nullptr);
        fbzz::scene::Script::SetPhysicsWorld(nullptr);
        m_editorApp.GetContext().activeScene = nullptr;
        m_editorApp.Shutdown();
        m_scene.reset();
    }

private:
    struct FocusAnim {
        bool                active   = false;
        fbzz::math::Vector3 startPos = {};
        fbzz::math::Vector3 endPos   = {};
        fbzz::math::Vector3 target   = {};
        float               t        = 0.0f;
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

    void RenderSceneViewport(fbzz::renderer::ResourceHandle<fbzz::renderer::RenderTargetTag> sceneRT)
    {
        m_renderer.SetRenderTarget(sceneRT, m_resources);
        m_renderer.Clear({ 0.05f, 0.05f, 0.08f, 1.0f });

        auto sceneRenderSettings = m_editorApp.GetContext().projectSettings.render;
        sceneRenderSettings.selectedObjects.clear();
        sceneRenderSettings.selectedObjects.reserve(m_editorApp.GetContext().selectedEntities.size());
        for (fbzz::scene::EntityID id : m_editorApp.GetContext().selectedEntities)
            sceneRenderSettings.selectedObjects.push_back({ id.index, id.generation });
        sceneRenderSettings.showSkeleton    = m_editorApp.GetContext().showSkeleton;
        sceneRenderSettings.showGrid        = m_editorApp.GetContext().showGrid;
        sceneRenderSettings.showLightRange  = m_editorApp.GetContext().showLightRange;
        sceneRenderSettings.showConstraints = sceneRenderSettings.showColliders;

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
        uiOptions.context        = &m_sceneUICtx;
        fbzz::scene::RenderSystem(*m_scene, m_renderer, m_resources,
                                  m_debugCamera.camera, sceneRT, &sceneRenderSettings,
                                  fbzz::Layer::Everything, &uiOptions);

        if (m_editorApp.GetContext().projectSettings.render.showColliders) {
            fbzz::renderer::DebugDraw::BeginFrame(m_renderer, m_resources,
                                                   m_debugCamera.camera.GetViewProjection());
            fbzz::scene::ConstraintDebugDrawSystem(m_physicsWorld, m_renderer);
            if (m_scene)
                fbzz::scene::TerrainCollisionDebugDrawSystem(*m_scene, m_renderer,
                                                              m_debugCamera.camera.m_position);
            fbzz::renderer::DebugDraw::Flush();
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
        uiOptions.context        = &m_gameUICtx;
        const auto& editorCtx = m_editorApp.GetContext();
        // WHY: gameViewportOriginはImGuiのスクリーン座標なので、同じ座標系のMousePosを使う。
        const ImGuiIO& imguiIO = ImGui::GetIO();
        uiOptions.mouseInCanvasSpace = {
            imguiIO.MousePos.x - editorCtx.gameViewportOriginX,
            imguiIO.MousePos.y - editorCtx.gameViewportOriginY
        };
        const bool mouseInGameViewport = uiOptions.mouseInCanvasSpace.x >= 0.0f
            && uiOptions.mouseInCanvasSpace.y >= 0.0f
            && uiOptions.mouseInCanvasSpace.x <= editorCtx.gameViewportWidth
            && uiOptions.mouseInCanvasSpace.y <= editorCtx.gameViewportHeight;
        // Editor UI のクリックをゲームへ漏らさず、Play 中の Game Viewport だけを操作対象にする。
        uiOptions.mousePressed = editorCtx.playMode->IsPlaying()
            && mouseInGameViewport && imguiIO.MouseDown[0];
        fbzz::scene::Scene* activeScene = m_editorApp.GetSceneManager().GetActive();
        if (!activeScene) return;
        fbzz::scene::RenderSystem(*activeScene, m_renderer, m_resources,
                                  gameCamera, gameRT, &gameRenderSettings,
                                  gameCullingMask, &uiOptions);
    }

    void RenderEditorPanels()
    {
        m_renderer.SetRenderTarget(
            fbzz::renderer::ResourceHandle<fbzz::renderer::RenderTargetTag>{}, m_resources);
        m_renderer.Clear({ 0.02f, 0.02f, 0.02f, 1.0f });
        m_editorApp.GetContext().activeScene = m_editorApp.GetSceneManager().GetActive();
        m_editorApp.RenderPanels(m_editorApp.GetContext());
        m_editorApp.EndFrame(m_imguiRenderer);
    }

    fbzz::renderer::IRenderer&          m_renderer;
    fbzz::renderer::IImGuiRenderer&     m_imguiRenderer;
    fbzz::renderer::ResourceManager&    m_resources;
    const LaunchProject&                m_project;
    fbzz::editor::EditorApp             m_editorApp;
    std::unique_ptr<fbzz::scene::Scene> m_scene;
    fbzz::physics::World                m_physicsWorld;
    fbzz::renderer::DebugCamera         m_debugCamera;
    FocusAnim                           m_focusAnim;
    fbzz::scene::UISystemContext        m_sceneUICtx;
    fbzz::scene::UISystemContext        m_gameUICtx;
    float                               m_frameDt   = 0.0f;
    bool                                m_stepFrame = false;
};
#endif

// ============================================================
// 起動関数
// ============================================================

/// ProjectSettings からウィンドウ設定を構築して StandaloneModule を起動する。
/// WHY: Application::Init() 前にウィンドウサイズを決定し、Renderer 初期化時点で正しいバックバッファを作る。
[[nodiscard]] int RunStandalone(fbzz::core::Application& app, const LaunchProject& project)
{
    fbzz::ProjectSettings settings;
    if (!settings.Load(PathToUtf8(project.settingsFile))) {
        MessageBoxW(nullptr, L"Failed to load ProjectSettings.", L"{{PROJECT_NAME}}", MB_OK | MB_ICONERROR);
        return 1;
    }

    // WHY: ProjectSettings の renderer 指定 (dx11/dx12) でレンダラーを生成する
    //      (--renderer= があれば Application::Init 内でそちらが優先)。
    if (!app.Init(fbzz::scene::MakeWindowConfig(settings), settings.app.rendererBackend)) return 1;

    auto& renderer = app.GetRenderer();
    fbzz::renderer::ResourceManager resources(renderer);
    fbzz::asset::AssetManager::Init(resources, PathToUtf8(project.root / L"Assets") + "/");

    fbzz::scene::StandaloneProjectModule module(
        renderer, resources, project.root, project.sceneFile, settings);
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
    // WHY: Editor も起動時プロジェクトの renderer 設定 (dx11/dx12) に従う (--renderer= 優先)。
    //      レンダラーはプロジェクト読込前に生成するため設定をここで先読みする。
    fbzz::ProjectSettings settings;
    settings.Load(PathToUtf8(project.settingsFile));
    if (!app.Init(fbzz::core::Window::Config{}, settings.app.rendererBackend)) return 1;

    auto& renderer = app.GetRenderer();
    auto& imguiRenderer = app.GetImGuiRenderer();
    fbzz::renderer::ResourceManager resources(renderer);
    fbzz::asset::AssetManager::Init(resources, PathToUtf8(project.root / L"Assets") + "/");

    fbzz::editor::EditorApp editorApp;
    if (!editorApp.Init(renderer, imguiRenderer, resources, app.GetWindow())) return 1;
    if (!editorApp.OpenProject(PathToUtf8(project.root),
                               PathToUtf8(project.settingsFile),
                               PathToUtf8(project.sceneFile))) return 1;
    app.Run(editorApp);
    return 0;
#endif
}

} // namespace

int Run()
{
    // WHY: FBZZEngine.dll は実行中ロックされ再ビルドできない。Engine ソースが古い DLL より
    //      新しければ、ここで一旦終了して cmake 再ビルド → 再起動を予約する (開発ビルドのみ)。
    if (fbzz::core::CheckEngineFreshnessAndRelaunch())
        return 0;

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

    const std::filesystem::path executableDirectory = GetExecutableDirectory();
    const std::filesystem::path workingDirectory = Exists(executableDirectory / L".fbzz_proj")
        ? executableDirectory
        : project.root;
    // WHY: 配布物は exe 隣に Assets があるため exeDir を CWD にする。
    //      Editor の Tools > Standalone から Binaries/Development の exe を起動する場合は
    //      Assets が project.root にあるため、相対 shader path が解決できるよう CWD を切り替える。
    SetCurrentDirectoryW(workingDirectory.wstring().c_str());

    // game.log を exe 隣に生成する (Standalone 時のみ)
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
    } logSink;
#ifdef FBZZ_STANDALONE_TARGET
    const bool isStandalone = true;
#else
    const bool isStandalone = args.standalone;
#endif
    if (isStandalone) {
        const std::filesystem::path logPath = GetExecutableDirectory() / L"game.log";
        logSink.file = fbzz::util::FileSystem::OpenBinaryWriter(logPath);
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
