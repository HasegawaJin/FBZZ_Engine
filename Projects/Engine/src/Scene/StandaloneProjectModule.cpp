/// @file    StandaloneProjectModule.cpp
/// @brief   Standaloneプロジェクト共通のゲーム更新・描画モジュール実装。
/// @author  Hasegawa Jin
/// @date    2026-06-22
#include <Engine/Scene/StandaloneProjectModule.hpp>
#include <Engine/Asset/RenderPipelineAsset.hpp>

#include <Engine/Audio/AudioManager.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/Cursor.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Profiler/ProfileScope.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Managed/ManagedGamePackage.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Scene/SceneUtils.hpp>
#include <Engine/Scene/Systems/RenderSystem.hpp>
#include <Physics/Layer.hpp>

#include <utility>

namespace fbzz::scene {

StandaloneProjectModule::StandaloneProjectModule(renderer::IRenderer& renderer,
                                                 renderer::ResourceManager& resources,
                                                 std::filesystem::path projectRoot,
                                                 std::filesystem::path startSceneFile,
                                                 ProjectSettings& settings)
    : m_renderer(renderer)
    , m_resources(resources)
    , m_projectRoot(std::move(projectRoot))
    , m_startSceneFile(std::move(startSceneFile))
    , m_settings(settings)
{
}

StandaloneProjectModule::StandaloneProjectModule(renderer::IRenderer& renderer,
                                                 renderer::ResourceManager& resources,
                                                 std::filesystem::path projectRoot,
                                                 std::filesystem::path startSceneFile,
                                                 ProjectSettings& settings,
                                                 bool exactStartScene)
    : StandaloneProjectModule(renderer, resources, std::move(projectRoot), std::move(startSceneFile), settings)
{
    m_exactStartScene = exactStartScene;
}

bool StandaloneProjectModule::OnInit()
{
    m_startedSuccessfully = false;
    std::string diagnostic;
    if (!InitializeManagedGame(m_projectRoot, util::FileSystem::GetExecutableDirectory() / L"Managed",
        FBZZ_CMAKE_CONFIG, m_managedLoaded, diagnostic)) {
        FBZZ_LOG_ERROR("Standalone C# startup failed: %s", diagnostic.c_str());
        return false;
    }
    m_runtime.ApplySettings(m_settings);
    m_runtime.RegisterScenes(m_projectRoot, m_resources);
    if (m_exactStartScene) {
        /// @note Register last so the exact selected path wins even when two scene files share a stem.
        const std::string name = util::FileSystem::PathToUtf8(m_startSceneFile.stem());
        m_runtime.GetSceneManager().RegisterFromFile(name, util::FileSystem::PathToUtf8(m_startSceneFile), m_resources);
    }
    m_runtime.LoadScene(m_startSceneFile);
    auto& app = core::Application::Get();
    /// @note GameHubを経由しない各プロジェクトのStandaloneでもScene Audioを有効にする。
    m_runtime.GetSceneManager().SetAudioManager(app.GetAudioManager());
    if (auto* audioManager = app.GetAudioManager()) {
        audioManager->SetVoiceLimit(static_cast<size_t>(m_settings.audio.voiceLimit));
        audioManager->ApplyBusLayout(m_settings.audio.BuildBusLayout());
    }
    /// @note 起動時だけアセットを展開し、以後の graphics 設定は実行用コピーへ適用する。
    renderer::RenderSettings resolvedSettings;
    (void)asset::ResolveRenderPipelineSettings(m_settings.render,
        m_settings.renderPipelineAssetPath, resolvedSettings);
    m_settings.render = std::move(resolvedSettings);
    app.SetActiveRenderSettings(&m_settings.render);
    /// @note カーソルの拘束と表示はスクリプトが名乗る (cursor.Push)。設定が持つのは «絵» だけ。
    m_settings.cursor.Apply(util::FileSystem::PathToUtf8(m_projectRoot));
    m_runtime.ActivateScriptRuntime(
        m_renderer, app.GetWindow().GetWidth(), app.GetWindow().GetHeight());
    core::Cursor::SetSuppressed(false);
    core::Cursor::ClearClipRegion();
    (void)core::Cursor::AcquireGameInput();
    m_waitingForStartScene = m_exactStartScene;
    m_startedSuccessfully = !m_exactStartScene;
    return true;
}

void StandaloneProjectModule::OnUpdate(float dt)
{
    /// @note Locked は中央へ戻す処理そのものがここにあり、Confined も他アプリが ClipCursor を
    /// @note Confined は他アプリの ClipCursor で外れるため毎フレーム張り直す。
    /// @note Input::Update の直後・スクリプトの前に解くことで Locked の移動量を当該フレームで読める。
    if (input::Input::PhysicalKeyHeld(input::KeyCode::SHIFT) && input::Input::PhysicalKeyDown(input::KeyCode::F1))
        core::Cursor::ReleaseGameInput(core::CursorSuspendReason::RELEASED);
    else if (!core::Cursor::IsGameInputOwned() && core::Cursor::GetState().windowActive
             && input::Input::PhysicalMouseButtonDown(0))
        (void)core::Cursor::AcquireGameInput();
    core::Cursor::ApplyLock();
    m_runtime.Update(dt, m_settings, true);
    if (m_waitingForStartScene) {
        m_waitingForStartScene = false;
        m_startedSuccessfully = m_runtime.GetActiveScene() != nullptr;
        if (!m_startedSuccessfully) {
            FBZZ_LOG_ERROR("Standalone Play could not load its selected scene.");
            core::Application::Get().Quit();
        }
    }
}

void StandaloneProjectModule::OnLateUpdate(float dt)
{
    m_runtime.LateUpdate(dt);
}

void StandaloneProjectModule::OnRender()
{
    auto& app = core::Application::Get();

    {
        FBZZ_PROFILE_SCOPE("Renderer::BeginFrame");
        m_renderer.BeginFrame();
    }
    m_renderer.SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>{}, m_resources);
    m_renderer.Clear({ 0.02f, 0.02f, 0.05f, 1.0f });

    const uint32_t width = app.GetWindow().GetWidth();
    const uint32_t height = app.GetWindow().GetHeight();
    m_runtime.UpdateScriptViewport(width, height);
    const float aspect = height > 0
        ? static_cast<float>(width) / static_cast<float>(height)
        : 1.0f;

    Scene* activeScene = m_runtime.GetActiveScene();
    if (!activeScene) {
        FBZZ_PROFILE_SCOPE("Renderer::EndFrame");
        m_renderer.EndFrame();
        return;
    }

    const renderer::Camera gameCamera = ResolveGameCamera(*activeScene, aspect);
    RenderSystemUIOptions uiOptions{};
    uiOptions.enabled = true;
    uiOptions.gameplayInput = true;
    uiOptions.viewportWidth = static_cast<float>(width);
    uiOptions.viewportHeight = static_cast<float>(height);
    uiOptions.mouseInCanvasSpace = input::Input::MousePosition();
    uiOptions.mousePressed = input::Input::MouseButton(0);
    uiOptions.targetView = UIRenderTargetView::GameViewport;
    uiOptions.context = &m_runtime.GetGameUIContext();

    RenderSystem(*activeScene, m_renderer, m_resources, gameCamera, {}, &m_settings.render,
                 Layer::Everything, &uiOptions);
    {
        FBZZ_PROFILE_SCOPE("Renderer::EndFrame");
        m_renderer.EndFrame();
    }
}

void StandaloneProjectModule::OnShutdown()
{
    /// @note PhysicsProxy が破棄済み World を参照しないよう、managed runtime を止める前に scene context を解除する。
    m_runtime.Shutdown();
    if (m_managedLoaded) {
        ManagedScriptRuntime::Instance().Unload();
        ManagedScriptRuntime::Instance().Shutdown();
        m_managedLoaded = false;
    }
    core::Application::Get().SetActiveRenderSettings(nullptr);
    core::Cursor::ResetForEditor();
}

} /// @note namespace fbzz::scene
