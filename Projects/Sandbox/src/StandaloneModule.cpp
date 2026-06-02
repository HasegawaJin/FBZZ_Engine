// FBZZ Engine
// StandaloneModule.cpp | fbzz::sandbox
// Sandbox のスタンドアロンゲーム実行 Module
#include "StandaloneModule.hpp"

#include "ModuleUtils.hpp"
#include "Util/PathUtil.hpp"

#include <Engine/Core/Application.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Profiler/ProfileScope.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/Systems/AnimatorSystem.hpp>
#include <Engine/Scene/Systems/IKSystem.hpp>
#include <Engine/Scene/Systems/PhysicsSystem.hpp>
#include <Engine/Scene/Systems/RenderSystem.hpp>
#include <Engine/Scene/Systems/ScriptSystem.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Physics/Layer.hpp>

#include <string>

namespace fbzz::sandbox {

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
    const std::string scenePathUtf8 = util::PathToUtf8(m_project.sceneFile);
    if (!scene::SceneSerializer::LoadInPlace(*m_scene, scenePathUtf8, m_resources)) {
        FBZZ_LOG_ERROR("Sandbox Standalone: failed to load scene: %s", scenePathUtf8.c_str());
        return false;
    }

    ApplyPhysicsSettings(m_physicsWorld, m_settings);
    ApplyUISettings(m_settings);
    m_physicsAccumulator = 0.0f;
    return true;
}

void StandaloneModule::OnUpdate(float dt)
{
    scene::Script::SetPhysicsWorld(&m_physicsWorld);
    scene::ScriptSystem(*m_scene, dt);
    {
        FBZZ_PROFILE_SCOPE("TransformSystem");
        scene::TransformSystem(*m_scene);
    }

    // WHAT: ProjectSettings の Hz に従って固定タイムステップ物理を複数回進める。
    // WHY: 描画 FPS が揺れても物理解の安定性を保つため、蓄積時間は最大 8 step に制限する。
    const int   physicsHz = m_settings.physics.hz < 1 ? 60 : m_settings.physics.hz;
    const float fixedDt   = 1.0f / static_cast<float>(physicsHz);
    m_physicsAccumulator += dt;
    const float maxAccum  = fixedDt * 8.0f;
    if (m_physicsAccumulator > maxAccum) m_physicsAccumulator = maxAccum;
    while (m_physicsAccumulator >= fixedDt) {
        scene::PhysicsSystem(*m_scene, m_physicsWorld, fixedDt);
        m_physicsAccumulator -= fixedDt;
    }
    {
        FBZZ_PROFILE_SCOPE("TransformSystem");
        scene::TransformSystem(*m_scene);
    }
}

void StandaloneModule::OnLateUpdate(float dt)
{
    scene::LateScriptSystem(*m_scene, dt);
    scene::AnimatorSystem(*m_scene, m_resources, dt);
    scene::IKSystem(*m_scene, m_physicsWorld, m_resources, dt);
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
    const renderer::Camera gameCamera = ResolveGameCamera(*m_scene, aspect);

    scene::RenderSystemUIOptions uiOptions{};
    uiOptions.enabled = true;
    uiOptions.viewportWidth = static_cast<float>(w);
    uiOptions.viewportHeight = static_cast<float>(h);
    uiOptions.mouseInCanvasSpace = input::Input::MousePosition();
    uiOptions.mousePressed = input::Input::MouseButton(0);
    uiOptions.targetView = scene::UIRenderTargetView::GameViewport;
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
