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
#include <Engine/Core/Time.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/ProjectResolver.hpp>
#include <Engine/ProjectSettings.hpp>
#include <Engine/Profiler/ProfileScope.hpp>
#include <Engine/Profiler/Profiler.hpp>
#include <Engine/Renderer/DebugCamera.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/AnimatorSystem.hpp>
#include <Engine/Scene/Systems/DebugDrawSystem.hpp>
#include <Engine/Scene/Systems/IKSystem.hpp>
#include <Engine/Scene/Systems/PhysicsSystem.hpp>
#include <Engine/Scene/Systems/RenderSystem.hpp>
#include <Engine/Scene/Systems/ScriptSystem.hpp>
#include <Engine/Scene/Systems/LifetimeSystem.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/SceneUtils.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <Editor/EditorApp.hpp>
#include <Physics/Layer.hpp>
#include <Physics/World.hpp>

#include <Windows.h>
#include <shellapi.h>
#include <filesystem>
#include <memory>
#include <string>

namespace fbzz::editor_launcher {

namespace {

using fbzz::util::FileSystem;
using fbzz::util::StringUtils;

const char* PhysicsStepCountMarkerName(int steps)
{
    // WHY: PhysicsSystem は固定タイムステップの catch-up で 1 描画フレームに複数回呼ばれる。
    //      Profiler 上で「重複呼び出し」か「意図した catch-up」かを即座に判別できるようにする。
    if (steps <= 1) return nullptr;
    if (steps == 2) return "PhysicsFixedSteps=2";
    if (steps == 3) return "PhysicsFixedSteps=3";
    if (steps == 4) return "PhysicsFixedSteps=4";
    if (steps == 5) return "PhysicsFixedSteps=5";
    if (steps == 6) return "PhysicsFixedSteps=6";
    if (steps == 7) return "PhysicsFixedSteps=7";
    return "PhysicsFixedSteps>=8";
}

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

void WarmupRenderResources(scene::Scene& scene,
                           renderer::IRenderer& renderer,
                           renderer::ResourceManager& resources,
                           editor::EditorApp& editorApp,
                           const renderer::Camera& sceneCamera,
                           const renderer::Camera& gameCamera,
                           fbzz::LayerMask gameCullingMask,
                           scene::UISystemContext& sceneUICtx,
                           scene::UISystemContext& gameUICtx)
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
        float w = 1920.0f, h = 1080.0f;
        if (auto* rt = resources.Get(sceneRT)) {
            w = static_cast<float>(rt->GetWidth());
            h = static_cast<float>(rt->GetHeight());
        }
        scene::RenderSystemUIOptions uiOptions{};
        uiOptions.enabled = true;
        uiOptions.viewportWidth = w;
        uiOptions.viewportHeight = h;
        uiOptions.targetView = scene::UIRenderTargetView::SceneViewport;
        uiOptions.context = &sceneUICtx;
        scene::RenderSystem(scene, renderer, resources, sceneCamera, sceneRT, &sceneRenderSettings,
                            fbzz::Layer::Everything, &uiOptions);
    }

    const auto gameRT = editorApp.GetGameViewportRT();
    if (gameRT.IsValid()) {
        renderer.SetRenderTarget(gameRT, resources);
        renderer.Clear({ 0.02f, 0.02f, 0.05f, 1.0f });
        float w = 1920.0f, h = 1080.0f;
        if (auto* rt = resources.Get(gameRT)) {
            w = static_cast<float>(rt->GetWidth());
            h = static_cast<float>(rt->GetHeight());
        }
        scene::RenderSystemUIOptions uiOptions{};
        uiOptions.enabled = true;
        uiOptions.viewportWidth = w;
        uiOptions.viewportHeight = h;
        uiOptions.targetView = scene::UIRenderTargetView::GameViewport;
        uiOptions.context = &gameUICtx;
        scene::RenderSystem(scene,
                            renderer,
                            resources,
                            gameCamera,
                            gameRT,
                            &editorApp.GetContext().projectSettings.render,
                            gameCullingMask,
                            &uiOptions);
    }

    // WHY: UI Viewport は Game View の完成済み RT を共有するため、ここで別 RT を描画しない。
    //      Warmup 時に UI 専用 RT を Clear すると、初期フレームで青い空 RT を表示する経路が残る。

    renderer.SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>{}, resources);
    renderer.Clear({ 0.02f, 0.02f, 0.02f, 1.0f });
    renderer.EndFrame();
}

// エディタモードのメインループ。
// WHY: Run() から切り出すことで Standalone 分岐が明確になり、
//      将来的なリファクタリングの境界をはっきりさせる。
void RunEditorLoop(renderer::IRenderer& renderer,
                   renderer::IImGuiRenderer& imguiRenderer,
                   renderer::ResourceManager& resources,
                   const fbzz::LaunchProject& project)
{
    auto& app = core::Application::Get();

    editor::EditorApp editorApp;
    if (!editorApp.Init(renderer, imguiRenderer, resources, app.GetWindow())) {
        app.Shutdown();
        return;
    }

    auto scene = std::make_unique<scene::Scene>();
    editorApp.GetContext().activeScene = scene.get();
    if (!editorApp.OpenProject(StringUtils::PathToUtf8(project.root),
                               StringUtils::PathToUtf8(project.settingsFile),
                               StringUtils::PathToUtf8(project.sceneFile))) {
        editorApp.Shutdown();
        return;
    }

    physics::World physicsWorld;
    scene::ApplyPhysicsSettings(physicsWorld, editorApp.GetContext().projectSettings);
    scene::UISystemContext sceneUICtx;
    scene::UISystemContext gameUICtx;
    scene::ApplyUISettings(editorApp.GetContext().projectSettings, &gameUICtx);
    scene::ApplyUISettings(editorApp.GetContext().projectSettings, &sceneUICtx);
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

        const auto gameRT = editorApp.GetGameViewportRT();
        float warmupAspect = debugCamera.camera.m_aspect;
        if (auto* rt = resources.Get(gameRT)) {
            warmupAspect = static_cast<float>(rt->GetWidth()) / static_cast<float>(rt->GetHeight());
        }
        renderer::Camera gameCamera = scene::ResolveEditorGameCamera(*scene, debugCamera.camera, warmupAspect);
        fbzz::LayerMask gameCullingMask = scene::ResolveGameCullingMask(*scene);

        WarmupRenderResources(*scene, renderer, resources, editorApp, debugCamera.camera, gameCamera, gameCullingMask, sceneUICtx, gameUICtx);

        // WHY: warmup にかかった時間を最初の DeltaTime / FPS 表示へ混ぜない。
        // WHAT: Time をここで初期化し、メインループの次フレームから通常計測を始める。
        Time::Tick();
    }

    while (app.IsRunning()) {
        Time::Tick();
        input::Input::Update();
        app.GetWindow().PollEvents();
        if (app.GetWindow().ShouldClose()) {
            app.Quit();
            break;
        }

        profiler::Profiler::BeginFrame();

        const float dt = Time::deltaTime;
        editorApp.BeginFrame();

        auto* playMode = editorApp.GetContext().playMode;
        if (playMode->ApplyPendingRestore(*scene)) {
            // WHY: World は物理同期とは別に m_contactCache / m_prevEvents を保持する。
            //      前 Play セッションの Collider* が残ったまま次 Play が始まると物理が誤動作するため、
            //      Stop 復元のタイミングで World を丸ごとリセットする。
            physicsWorld = physics::World{};
            scene::ApplyPhysicsSettings(physicsWorld, editorApp.GetContext().projectSettings);
            physicsAccumulator = 0.0f;
        }

        if (!playMode->IsPlaying()) {
            // WHY: DebugCamera の速度・感度は EditorContext に保持され、
            //      EditorSettings によって起動間で永続化される。
            //      毎フレーム適用することで、将来的に設定 UI からリアルタイム変更できる。
            const auto& ctx = editorApp.GetContext();
            debugCamera.moveSpeed = ctx.cameraSpeed;
            debugCamera.mouseSens = ctx.cameraSensitivity;
            debugCamera.Update(dt, ctx.sceneViewportHovered);
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
        const float simulationDt = stepFrame ? (1.0f / 60.0f) : dt;
        if (playMode->IsPlaying() || stepFrame) {
            const auto& settings = editorApp.GetContext().projectSettings;
            scene::ApplyPhysicsSettings(physicsWorld, settings);
            scene::Script::SetPhysicsWorld(&physicsWorld);
            scene::ScriptSystem(*scene, simulationDt);
            scene::TransformSystem(*scene);

            const int physicsHz = settings.physics.hz < 1 ? 1 : settings.physics.hz;
            const float fixedDt = 1.0f / static_cast<float>(physicsHz);

            int physicsStepsThisFrame = 0;
            if (stepFrame) {
                FBZZ_PROFILE_SCOPE("PhysicsFixedStepLoop");
                scene::PhysicsSystem(*scene, physicsWorld, fixedDt);
                physicsStepsThisFrame = 1;
            } else {
                FBZZ_PROFILE_SCOPE("PhysicsFixedStepLoop");
                physicsAccumulator += dt;
                const float maxAccumulatedTime = fixedDt * 8.0f;
                if (physicsAccumulator > maxAccumulatedTime)
                    physicsAccumulator = maxAccumulatedTime;
                while (physicsAccumulator >= fixedDt) {
                    scene::PhysicsSystem(*scene, physicsWorld, fixedDt);
                    physicsAccumulator -= fixedDt;
                    ++physicsStepsThisFrame;
                }
            }
            if (const char* marker = PhysicsStepCountMarkerName(physicsStepsThisFrame))
                FBZZ_PROFILE_MARKER(marker);
            scene::TransformSystem(*scene);
            scene::LateScriptSystem(*scene, simulationDt);
            scene::LifetimeSystem(*scene, simulationDt);
            {
                FBZZ_PROFILE_SCOPE("Scene::FlushDestroyQueue");
                scene->FlushDestroyQueue(simulationDt);
            }
        } else {
            physicsAccumulator = 0.0f;
        }
        scene::AnimatorSystem(*scene, resources, simulationDt);
        scene::IKSystem(*scene, physicsWorld, resources, simulationDt);

        const auto sceneRT = editorApp.GetViewportRT();
        const auto gameRT = editorApp.GetGameViewportRT();

        if (auto* rt = resources.Get(sceneRT)) {
            debugCamera.camera.m_aspect = static_cast<float>(rt->GetWidth()) / static_cast<float>(rt->GetHeight());
        }

        float gameAspect = debugCamera.camera.m_aspect;
        if (auto* rt = resources.Get(gameRT)) {
            gameAspect = static_cast<float>(rt->GetWidth()) / static_cast<float>(rt->GetHeight());
        }
        renderer::Camera gameCamera = scene::ResolveEditorGameCamera(*scene, debugCamera.camera, gameAspect);
        fbzz::LayerMask gameCullingMask = scene::ResolveGameCullingMask(*scene);

        renderer.BeginFrame();

        // WHY: UI Viewport は Game View の完成済み RT を共有する。
        //      UI 用に RenderSystem を二重実行すると、Clear 済みの別 RT を表示する不整合が起きる。

        renderer.SetRenderTarget(sceneRT, resources);
        renderer.Clear({ 0.05f, 0.05f, 0.08f, 1.0f });
        auto sceneRenderSettings = editorApp.GetContext().projectSettings.render;
        sceneRenderSettings.selectedObjects.clear();
        sceneRenderSettings.selectedObjects.reserve(editorApp.GetContext().selectedEntities.size());
        for (scene::EntityID id : editorApp.GetContext().selectedEntities)
            sceneRenderSettings.selectedObjects.push_back({ id.index, id.generation });
        // WHY: Scene ビューは 3D 編集用途なので、画面固定 UI は CanvasEditor へ分離する。
        //      ただし WorldSpace Canvas は 3D シーン内オブジェクトなので SceneViewport に重ねて描く。
        {
            float w = 1920.0f, h = 1080.0f;
            if (auto* rt = resources.Get(sceneRT)) {
                w = static_cast<float>(rt->GetWidth());
                h = static_cast<float>(rt->GetHeight());
            }
            scene::RenderSystemUIOptions uiOptions{};
            uiOptions.enabled = true;
            uiOptions.viewportWidth = w;
            uiOptions.viewportHeight = h;
            uiOptions.targetView = scene::UIRenderTargetView::SceneViewport;
            uiOptions.context = &sceneUICtx;
            scene::RenderSystem(*scene,
                                renderer,
                                resources,
                                debugCamera.camera,
                                sceneRT,
                                &sceneRenderSettings,
                                fbzz::Layer::Everything,
                                &uiOptions);
        }
        if (editorApp.GetContext().projectSettings.render.showColliders) {
            renderer::DebugDraw::BeginFrame(renderer, resources, debugCamera.camera.GetViewProjection());
            scene::ConstraintDebugDrawSystem(physicsWorld, renderer);
            renderer::DebugDraw::Flush();
        }
        if (editorApp.GetContext().showSkeleton) {
            scene::AnimatorDebugDrawSystem(*scene, renderer, resources, debugCamera.camera.GetViewProjection());
        }
        if (editorApp.GetContext().showGrid) {
            scene::GridDebugDrawSystem(renderer, resources, debugCamera.camera.GetViewProjection());
        }
        if (editorApp.GetContext().showLightRange) {
            scene::LightRangeDebugDrawSystem(*scene, renderer, resources,
                                             debugCamera.camera.GetViewProjection());
        }

        if (gameRT.IsValid()) {
            renderer.SetRenderTarget(gameRT, resources);
            renderer.Clear({ 0.02f, 0.02f, 0.05f, 1.0f });
            {
                float w = 1920.0f, h = 1080.0f;
                if (auto* rt = resources.Get(gameRT)) {
                    w = static_cast<float>(rt->GetWidth());
                    h = static_cast<float>(rt->GetHeight());
                }
                scene::RenderSystemUIOptions uiOptions{};
                uiOptions.enabled = true;
                uiOptions.viewportWidth = w;
                uiOptions.viewportHeight = h;
                uiOptions.targetView = scene::UIRenderTargetView::GameViewport;
                uiOptions.context = &gameUICtx;
                scene::RenderSystem(*scene,
                                    renderer,
                                    resources,
                                    gameCamera,
                                    gameRT,
                                    &editorApp.GetContext().projectSettings.render,
                                    gameCullingMask,
                                    &uiOptions);
            }
        }

        renderer.SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>{}, resources);
        renderer.Clear({ 0.02f, 0.02f, 0.02f, 1.0f });
        editorApp.GetContext().activeScene = scene.get();
        editorApp.RenderPanels(editorApp.GetContext());
        editorApp.EndFrame(imguiRenderer);

        renderer.EndFrame();
        profiler::Profiler::EndFrame();
    }

    // WHY: FreeLibrary より前に全スクリプトの OnDestroy と destructor を
    //      DLL コードが有効なうちに実行する。Clear() 後は activeScene を nullptr に
    //      してダングリング参照を防ぎ、Unload(nullptr) で DestroyAllScripts をスキップする。
    scene->Clear();
    scene::Script::SetPhysicsWorld(nullptr);
    editorApp.GetContext().activeScene = nullptr;
    editorApp.Shutdown();
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

        auto& renderer = app.GetRenderer();
        auto& imguiRenderer = app.GetImGuiRenderer();
        renderer::ResourceManager resources(renderer);

        const std::filesystem::path assetRoot = project.root / L"Assets";
        asset::AssetManager::Init(resources, StringUtils::PathToUtf8(assetRoot) + "/");

        RunEditorLoop(renderer, imguiRenderer, resources, project);
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
