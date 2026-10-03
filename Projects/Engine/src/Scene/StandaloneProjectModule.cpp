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

bool StandaloneProjectModule::OnInit()
{
    m_runtime.ApplySettings(m_settings);
    m_runtime.RegisterScenes(m_projectRoot, m_resources);
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
    m_settings.cursor.Apply(m_projectRoot.string());
    m_runtime.ActivateScriptRuntime(
        m_renderer, app.GetWindow().GetWidth(), app.GetWindow().GetHeight());
    return true;
}

void StandaloneProjectModule::OnUpdate(float dt)
{
    /// @note Locked は中央へ戻す処理そのものがここにあり、Confined も他アプリが ClipCursor を
    /// @note Confined は他アプリの ClipCursor で外れるため毎フレーム張り直す。
    /// @note Input::Update の直後・スクリプトの前に解くことで Locked の移動量を当該フレームで読める。
    core::Cursor::ApplyLock();
    m_runtime.Update(dt, m_settings, true);
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
    /// @note Script DLL を解放する前にスクリプトの仮想デストラクターを実行し、
    /// @note PhysicsProxy が破棄済み World を参照しないようコンテキストも解除する。
    m_runtime.Shutdown();
    core::Application::Get().SetActiveRenderSettings(nullptr);
}

} /// @note namespace fbzz::scene
