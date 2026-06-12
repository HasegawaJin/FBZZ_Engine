// FBZZ Engine
// StandaloneModule.cpp | fbzz::sandbox
// Sandbox のスタンドアロンゲーム実行 Module
#include "StandaloneModule.hpp"

#include <Engine/Core/Application.hpp>
#include <Engine/Scene/SceneUtils.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Profiler/ProfileScope.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/Systems/RenderSystem.hpp>
#include <Physics/Layer.hpp>

#include <string>

namespace fbzz::sandbox {

using fbzz::util::StringUtils;

StandaloneModule::StandaloneModule(renderer::IRenderer& renderer,
                                   renderer::ResourceManager& resources,
                                   const LaunchProject& project,
                                   const ProjectSettings& settings)
    : m_renderer(renderer)
    , m_resources(resources)
    , m_project(project)
    , m_settings(settings)
{
}

bool StandaloneModule::OnInit()
{
    m_scene = std::make_unique<scene::Scene>();
    const std::string scenePathUtf8 = StringUtils::PathToUtf8(m_project.sceneFile);
    if (!scene::SceneSerializer::LoadInPlace(*m_scene, scenePathUtf8, m_resources)) {
        FBZZ_LOG_ERROR("Sandbox Standalone: failed to load scene: %s", scenePathUtf8.c_str());
        return false;
    }

    scene::ApplyPhysicsSettings(m_physicsWorld, m_settings);
    scene::ApplyUISettings(m_settings, &m_uiCtx);
    m_sceneManager.SetScene(m_scene.get());
    m_sceneManager.SetPhysicsHz(m_settings.physics.hz);
    return true;
}

void StandaloneModule::OnUpdate(float dt)
{
    scene::Script::SetPhysicsWorld(&m_physicsWorld);
    m_sceneManager.Update(dt, m_physicsWorld);
}

void StandaloneModule::OnLateUpdate(float dt)
{
    m_sceneManager.LateUpdate(dt, m_physicsWorld);
}

void StandaloneModule::OnRender()
{
    auto& app = core::Application::Get();

    {
        FBZZ_PROFILE_SCOPE("Renderer::BeginFrame");
        m_renderer.BeginFrame();
    }
    m_renderer.SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>{}, m_resources);
    m_renderer.Clear({ 0.02f, 0.02f, 0.05f, 1.0f });

    const uint32_t w = app.GetWindow().GetWidth();
    const uint32_t h = app.GetWindow().GetHeight();
    const float aspect = (h > 0) ? (static_cast<float>(w) / static_cast<float>(h)) : 1.0f;
    const renderer::Camera gameCamera = scene::ResolveGameCamera(*m_scene, aspect);

    scene::RenderSystemUIOptions uiOptions{};
    uiOptions.enabled = true;
    uiOptions.viewportWidth = static_cast<float>(w);
    uiOptions.viewportHeight = static_cast<float>(h);
    uiOptions.mouseInCanvasSpace = input::Input::MousePosition();
    uiOptions.mousePressed = input::Input::MouseButton(0);
    uiOptions.targetView = scene::UIRenderTargetView::GameViewport;
    uiOptions.context = &m_uiCtx;
    scene::RenderSystem(*m_scene, m_renderer, m_resources, gameCamera, {}, &m_settings.render,
                        fbzz::Layer::Everything, &uiOptions);
    {
        FBZZ_PROFILE_SCOPE("Renderer::EndFrame");
        m_renderer.EndFrame();
    }
}

void StandaloneModule::OnShutdown()
{
    m_scene.reset();
}

} // namespace fbzz::sandbox
