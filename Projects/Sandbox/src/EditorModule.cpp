// FBZZ Engine
// EditorModule.cpp | fbzz::sandbox
// Sandbox のエディタ実行 Module
#include "EditorModule.hpp"

#include "ModuleUtils.hpp"
#include "Util/PathUtil.hpp"

#include <Editor/Util/SceneSerializer.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Profiler/ProfileScope.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Systems/AnimatorDebugDrawSystem.hpp>
#include <Engine/Scene/Systems/AnimatorSystem.hpp>
#include <Engine/Scene/Systems/ConstraintDebugDrawSystem.hpp>
#include <Engine/Scene/Systems/IKSystem.hpp>
#include <Engine/Scene/Systems/PhysicsSystem.hpp>
#include <Engine/Scene/Systems/RenderSystem.hpp>
#include <Engine/Scene/Systems/ScriptSystem.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Physics/Layer.hpp>

namespace fbzz::sandbox {

EditorModule::EditorModule(renderer::IRenderer& renderer,
                           renderer::ResourceManager& resources,
                           const LaunchProject& project)
    : m_renderer(renderer)
    , m_resources(resources)
    , m_project(project)
{
}

bool EditorModule::OnInit()
{
    auto& app = core::Application::Get();
    if (!m_editorApp.Init(m_renderer, m_resources, app.GetWindow())) {
        return false;
    }

    m_scene = std::make_unique<scene::Scene>();
    m_editorApp.GetContext().activeScene = m_scene.get();
    if (!m_editorApp.OpenProject(util::PathToUtf8(m_project.root),
                                 util::PathToUtf8(m_project.settingsFile),
                                 util::PathToUtf8(m_project.sceneFile))) {
        return false;
    }

    ApplyPhysicsSettings(m_physicsWorld, m_editorApp.GetContext().projectSettings);
    ApplyUISettings(m_editorApp.GetContext().projectSettings);
    m_physicsAccumulator = 0.0f;

    m_debugCamera.camera.m_position = { 0.0f, 2.5f, -8.0f };
    m_debugCamera.camera.m_aspect = 1920.0f / 1080.0f;
    m_editorApp.GetContext().editorCamera = &m_debugCamera.camera;
    return true;
}

void EditorModule::OnUpdate(float dt)
{
    m_frameDt = dt;
    {
        FBZZ_PROFILE_SCOPE("EditorApp::BeginFrame");
        m_editorApp.BeginFrame();
    }

    auto* playMode = m_editorApp.GetContext().playMode;
    if (playMode->ApplyPendingRestore(*m_scene)) {
        ApplyPhysicsSettings(m_physicsWorld, m_editorApp.GetContext().projectSettings);
        m_physicsAccumulator = 0.0f;
    }

    if (!playMode->IsPlaying()) {
        m_debugCamera.Update(dt);
    }
    UpdateFocusAnimation(dt);

    {
        FBZZ_PROFILE_SCOPE("TransformSystem");
        scene::TransformSystem(*m_scene);
    }

    m_stepFrame = playMode->ConsumeStep();
    const float simulationDt = SimulationDeltaTime();
    if (playMode->IsPlaying() || m_stepFrame) {
        const auto& settings = m_editorApp.GetContext().projectSettings;
        ApplyPhysicsSettings(m_physicsWorld, settings);
        scene::Script::SetPhysicsWorld(&m_physicsWorld);
        scene::ScriptSystem(*m_scene, simulationDt);
        {
            FBZZ_PROFILE_SCOPE("TransformSystem");
            scene::TransformSystem(*m_scene);
        }

        const int physicsHz = settings.physics.hz < 1 ? 1 : settings.physics.hz;
        const float fixedDt = 1.0f / static_cast<float>(physicsHz);
        if (m_stepFrame) {
            scene::PhysicsSystem(*m_scene, m_physicsWorld, fixedDt);
        } else {
            m_physicsAccumulator += dt;
            const float maxAccumulatedTime = fixedDt * 8.0f;
            if (m_physicsAccumulator > maxAccumulatedTime)
                m_physicsAccumulator = maxAccumulatedTime;
            while (m_physicsAccumulator >= fixedDt) {
                scene::PhysicsSystem(*m_scene, m_physicsWorld, fixedDt);
                m_physicsAccumulator -= fixedDt;
            }
        }
        {
            FBZZ_PROFILE_SCOPE("TransformSystem");
            scene::TransformSystem(*m_scene);
        }
        scene::LateScriptSystem(*m_scene, simulationDt);
    } else {
        m_physicsAccumulator = 0.0f;
    }
}

void EditorModule::OnLateUpdate(float)
{
    const float simulationDt = SimulationDeltaTime();
    scene::AnimatorSystem(*m_scene, m_resources, simulationDt);
    scene::IKSystem(*m_scene, m_physicsWorld, m_resources, simulationDt);
}

void EditorModule::OnRender()
{
    const auto sceneRT = m_editorApp.GetViewportRT();
    const auto gameRT = m_editorApp.GetGameViewportRT();

    if (auto* rt = m_resources.Get(sceneRT)) {
        m_debugCamera.camera.m_aspect = static_cast<float>(rt->GetWidth()) / static_cast<float>(rt->GetHeight());
    }

    float gameAspect = m_debugCamera.camera.m_aspect;
    if (auto* rt = m_resources.Get(gameRT)) {
        gameAspect = static_cast<float>(rt->GetWidth()) / static_cast<float>(rt->GetHeight());
    }

    renderer::Camera gameCamera = ResolveEditorGameCamera(gameAspect);
    fbzz::LayerMask gameCullingMask = ResolveGameCullingMask();

    {
        FBZZ_PROFILE_SCOPE("Renderer::BeginFrame");
        m_renderer.BeginFrame();
    }

    RenderSceneViewport(sceneRT);
    RenderGameViewport(gameRT, gameCamera, gameCullingMask);
    RenderEditorPanels();

    {
        FBZZ_PROFILE_SCOPE("Renderer::EndFrame");
        m_renderer.EndFrame();
    }
}

void EditorModule::OnShutdown()
{
    m_editorApp.Shutdown();
    m_scene.reset();
}

float EditorModule::SimulationDeltaTime() const
{
    return m_stepFrame ? (1.0f / 60.0f) : m_frameDt;
}

void EditorModule::UpdateFocusAnimation(float dt)
{
    auto& ctx = m_editorApp.GetContext();
    if (ctx.requestFocusOnSelected) {
        ctx.requestFocusOnSelected = false;
        const math::Vector3 target = ctx.focusTargetPosition;
        const math::Vector3 dir    = m_debugCamera.camera.m_position - target;
        const float dist           = dir.Length();
        constexpr float FOCUS_DISTANCE = 5.0f;
        const math::Vector3 camDir = (dist > 0.01f)
            ? dir * (1.0f / dist)
            : math::Vector3{ 0.0f, 0.5f, -1.0f }.Normalized();

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

renderer::Camera EditorModule::ResolveEditorGameCamera(float gameAspect)
{
    renderer::Camera gameCamera = m_debugCamera.camera;
    gameCamera.m_aspect = gameAspect;

    for (auto& go : m_scene->GameObjects()) {
        auto* cameraComponent = go.GetComponent<scene::CameraComponent>();
        if (!go.activeSelf() || !cameraComponent || !cameraComponent->enabled || !cameraComponent->isMain)
            continue;

        gameCamera.m_position = go.transform.position;
        gameCamera.m_rotation = go.transform.rotation;
        gameCamera.m_fovY     = cameraComponent->fovY;
        gameCamera.m_aspect   = gameAspect;
        gameCamera.m_near     = cameraComponent->nearZ;
        gameCamera.m_far      = cameraComponent->farZ;
        break;
    }

    return gameCamera;
}

fbzz::LayerMask EditorModule::ResolveGameCullingMask()
{
    for (auto& go : m_scene->GameObjects()) {
        auto* cameraComponent = go.GetComponent<scene::CameraComponent>();
        if (!go.activeSelf() || !cameraComponent || !cameraComponent->enabled || !cameraComponent->isMain)
            continue;
        return cameraComponent->cullingMask;
    }
    return fbzz::Layer::Everything;
}

void EditorModule::RenderSceneViewport(renderer::ResourceHandle<renderer::RenderTargetTag> sceneRT)
{
    m_renderer.SetRenderTarget(sceneRT, m_resources);
    m_renderer.Clear({ 0.05f, 0.05f, 0.08f, 1.0f });

    auto sceneRenderSettings = m_editorApp.GetContext().projectSettings.render;
    sceneRenderSettings.selectedObjects.clear();
    sceneRenderSettings.selectedObjects.reserve(m_editorApp.GetContext().selectedEntities.size());
    for (scene::EntityID id : m_editorApp.GetContext().selectedEntities)
        sceneRenderSettings.selectedObjects.push_back({ id.index, id.generation });

    float w = 1920.0f;
    float h = 1080.0f;
    if (auto* rt = m_resources.Get(sceneRT)) {
        w = static_cast<float>(rt->GetWidth());
        h = static_cast<float>(rt->GetHeight());
    }

    scene::RenderSystemUIOptions uiOptions{};
    uiOptions.enabled = true;
    uiOptions.viewportWidth = w;
    uiOptions.viewportHeight = h;
    uiOptions.targetView = scene::UIRenderTargetView::SceneViewport;
    scene::RenderSystem(*m_scene,
                        m_renderer,
                        m_resources,
                        m_debugCamera.camera,
                        sceneRT,
                        &sceneRenderSettings,
                        fbzz::Layer::Everything,
                        &uiOptions);

    if (m_editorApp.GetContext().projectSettings.render.showColliders) {
        renderer::DebugDraw::BeginFrame(m_renderer, m_resources, m_debugCamera.camera.GetViewProjection());
        scene::ConstraintDebugDrawSystem(m_physicsWorld, m_renderer);
        renderer::DebugDraw::Flush();
    }
    if (m_editorApp.GetContext().showSkeleton) {
        scene::AnimatorDebugDrawSystem(*m_scene, m_renderer, m_resources, m_debugCamera.camera.GetViewProjection());
    }
}

void EditorModule::RenderGameViewport(renderer::ResourceHandle<renderer::RenderTargetTag> gameRT,
                                      const renderer::Camera& gameCamera,
                                      fbzz::LayerMask gameCullingMask)
{
    if (!gameRT.IsValid()) return;

    m_renderer.SetRenderTarget(gameRT, m_resources);
    m_renderer.Clear({ 0.02f, 0.02f, 0.05f, 1.0f });
    auto gameRenderSettings = m_editorApp.GetContext().projectSettings.render;
    gameRenderSettings.viewMode = renderer::ViewMode::Lit;
    gameRenderSettings.showSelectionOutline = false;
    gameRenderSettings.selectedObjects.clear();

    float w = 1920.0f;
    float h = 1080.0f;
    if (auto* rt = m_resources.Get(gameRT)) {
        w = static_cast<float>(rt->GetWidth());
        h = static_cast<float>(rt->GetHeight());
    }

    scene::RenderSystemUIOptions uiOptions{};
    uiOptions.enabled = true;
    uiOptions.viewportWidth = w;
    uiOptions.viewportHeight = h;
    uiOptions.targetView = scene::UIRenderTargetView::GameViewport;
    scene::RenderSystem(*m_scene,
                        m_renderer,
                        m_resources,
                        gameCamera,
                        gameRT,
                        &gameRenderSettings,
                        gameCullingMask,
                        &uiOptions);
}

void EditorModule::RenderEditorPanels()
{
    m_renderer.SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>{}, m_resources);
    m_renderer.Clear({ 0.02f, 0.02f, 0.02f, 1.0f });
    m_editorApp.GetContext().activeScene = m_scene.get();
    {
        FBZZ_PROFILE_SCOPE("EditorApp::RenderPanels");
        m_editorApp.RenderPanels(m_editorApp.GetContext());
    }
    {
        FBZZ_PROFILE_SCOPE("EditorApp::EndFrame");
        m_editorApp.EndFrame(m_renderer);
    }
}

} // namespace fbzz::sandbox
