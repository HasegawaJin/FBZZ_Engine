// FBZZ Engine
// EditorModule.cpp | fbzz::sandbox
// Sandbox のエディタ実行 Module
#include "EditorModule.hpp"

#include <Engine/Scene/SceneUtils.hpp>
#include <Engine/Util/StringUtils.hpp>

#include <Editor/Util/SceneIO.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Profiler/ProfileScope.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Systems/AnimatorSystem.hpp>
#include <Engine/Scene/Systems/DebugDrawSystem.hpp>
#include <Engine/Scene/Systems/IKSystem.hpp>
#include <Engine/Scene/Systems/LifetimeSystem.hpp>
#include <Engine/Scene/Systems/PhysicsSystem.hpp>
#include <Engine/Scene/Systems/RenderSystem.hpp>
#include <Engine/Scene/Systems/ScriptSystem.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Physics/Layer.hpp>

namespace fbzz::sandbox {

using fbzz::util::StringUtils;

namespace {

const char* PhysicsStepCountMarkerName(int steps)
{
    // WHY: 固定タイムステップの catch-up で PhysicsSystem が 1 フレームに複数回走ることがある。
    //      Profiler marker として回数を残し、重複呼び出し疑いを確認しやすくする。
    if (steps <= 1) return nullptr;
    if (steps == 2) return "PhysicsFixedSteps=2";
    if (steps == 3) return "PhysicsFixedSteps=3";
    if (steps == 4) return "PhysicsFixedSteps=4";
    if (steps == 5) return "PhysicsFixedSteps=5";
    if (steps == 6) return "PhysicsFixedSteps=6";
    if (steps == 7) return "PhysicsFixedSteps=7";
    return "PhysicsFixedSteps>=8";
}

} // namespace

EditorModule::EditorModule(renderer::IRenderer& renderer,
                           renderer::IImGuiRenderer& imguiRenderer,
                           renderer::ResourceManager& resources,
                           const LaunchProject& project)
    : m_renderer(renderer)
    , m_imguiRenderer(imguiRenderer)
    , m_resources(resources)
    , m_project(project)
{
}

bool EditorModule::OnInit()
{
    auto& app = core::Application::Get();
    if (!m_editorApp.Init(m_renderer, m_imguiRenderer, m_resources, app.GetWindow())) {
        return false;
    }

    m_scene = std::make_unique<scene::Scene>();
    m_editorApp.GetContext().activeScene = m_scene.get();
    if (!m_editorApp.OpenProject(StringUtils::PathToUtf8(m_project.root),
                                 StringUtils::PathToUtf8(m_project.settingsFile),
                                 StringUtils::PathToUtf8(m_project.sceneFile))) {
        return false;
    }

    scene::ApplyPhysicsSettings(m_physicsWorld, m_editorApp.GetContext().projectSettings);
    scene::ApplyUISettings(m_editorApp.GetContext().projectSettings);
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
        // WHY: World は物理同期とは別に m_contactCache / m_prevEvents を保持する。
        //      前 Play セッションの Collider* が残ったまま次 Play が始まると物理が誤動作するため、
        //      Stop 復元のタイミングで World を丸ごとリセットする。
        m_physicsWorld = physics::World{};
        scene::ApplyPhysicsSettings(m_physicsWorld, m_editorApp.GetContext().projectSettings);
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
        scene::ApplyPhysicsSettings(m_physicsWorld, settings);
        scene::Script::SetPhysicsWorld(&m_physicsWorld);
        scene::ScriptSystem(*m_scene, simulationDt);
        {
            FBZZ_PROFILE_SCOPE("TransformSystem");
            scene::TransformSystem(*m_scene);
        }

        const int physicsHz = settings.physics.hz < 1 ? 1 : settings.physics.hz;
        const float fixedDt = 1.0f / static_cast<float>(physicsHz);
        int physicsStepsThisFrame = 0;
        if (m_stepFrame) {
            FBZZ_PROFILE_SCOPE("PhysicsFixedStepLoop");
            scene::PhysicsSystem(*m_scene, m_physicsWorld, fixedDt);
            physicsStepsThisFrame = 1;
        } else {
            FBZZ_PROFILE_SCOPE("PhysicsFixedStepLoop");
            m_physicsAccumulator += dt;
            const float maxAccumulatedTime = fixedDt * 8.0f;
            if (m_physicsAccumulator > maxAccumulatedTime)
                m_physicsAccumulator = maxAccumulatedTime;
            while (m_physicsAccumulator >= fixedDt) {
                scene::PhysicsSystem(*m_scene, m_physicsWorld, fixedDt);
                m_physicsAccumulator -= fixedDt;
                ++physicsStepsThisFrame;
            }
        }
        if (const char* marker = PhysicsStepCountMarkerName(physicsStepsThisFrame))
            FBZZ_PROFILE_MARKER(marker);
        {
            FBZZ_PROFILE_SCOPE("TransformSystem");
            scene::TransformSystem(*m_scene);
        }
        scene::LateScriptSystem(*m_scene, simulationDt);
        scene::LifetimeSystem(*m_scene, simulationDt);
        {
            FBZZ_PROFILE_SCOPE("Scene::FlushDestroyQueue");
            m_scene->FlushDestroyQueue(simulationDt);
        }
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

    renderer::Camera gameCamera = scene::ResolveEditorGameCamera(*m_scene, m_debugCamera.camera, gameAspect);
    fbzz::LayerMask gameCullingMask = scene::ResolveGameCullingMask(*m_scene);

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
    if (m_editorApp.GetContext().showGrid) {
        scene::GridDebugDrawSystem(m_renderer, m_resources, m_debugCamera.camera.GetViewProjection());
    }
    if (m_editorApp.GetContext().showLightRange) {
        scene::LightRangeDebugDrawSystem(*m_scene, m_renderer, m_resources,
                                         m_debugCamera.camera.GetViewProjection());
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
        m_editorApp.EndFrame(m_imguiRenderer);
    }
}

} // namespace fbzz::sandbox
