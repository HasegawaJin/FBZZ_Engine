// FBZZ Engine
// StandaloneProjectModule.cpp | fbzz::scene
// Standaloneプロジェクト共通のゲーム更新・描画モジュール実装
#include <Engine/Scene/StandaloneProjectModule.hpp>

#include <Engine/Core/Application.hpp>
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
                                                 const ProjectSettings& settings)
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
    // GameHubを経由しない各プロジェクトのStandaloneでもScene Audioを有効にする。
    m_runtime.GetSceneManager().SetAudioManager(app.GetAudioManager());
    m_runtime.ActivateScriptRuntime(
        m_renderer, app.GetWindow().GetWidth(), app.GetWindow().GetHeight());
    return true;
}

void StandaloneProjectModule::OnUpdate(float dt)
{
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
    // WHY: Script DLLを解放する前にスクリプトの仮想デストラクターを実行し、
    //      PhysicsProxyが破棄済みWorldを参照しないようコンテキストも解除する。
    m_runtime.Shutdown();
}

} // namespace fbzz::scene
