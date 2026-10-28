/// @file    ProjectRuntime.cpp
/// @brief   Editor PlayとStandaloneで共有するプロジェクト実行パイプライン実装。
/// @author  Hasegawa Jin
/// @date    2026-06-22
#include <Engine/Scene/ProjectRuntime.hpp>

#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Scene/ProjectLauncher.hpp>
#include <Engine/Scene/SceneUtils.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/Window.hpp>
#include <Engine/Util/StringUtils.hpp>

namespace fbzz::scene {
ProjectRuntime::~ProjectRuntime()
{
    m_scriptRuntime.requestDisplay = nullptr; m_scriptRuntime.displayContext = nullptr;
    SetScriptQuitHandler(nullptr, nullptr);
    if (ScriptRuntime::GetOverride() == &m_scriptRuntime) ScriptRuntime::Override(nullptr);
}

void ProjectRuntime::ApplySettings(const ProjectSettings& settings)
{
    ProjectLauncher::ApplySettings(
        m_physicsWorld, m_sceneManager, settings, &m_gameUICtx);
}

void ProjectRuntime::ApplyAdditionalUIContext(const ProjectSettings& settings,
                                              UISystemContext& context)
{
    ProjectLauncher::ApplyAdditionalUIContext(settings, context);
}

void ProjectRuntime::RegisterScenes(const std::filesystem::path& projectRoot,
                                    renderer::ResourceManager& resources)
{
    ProjectLauncher::RegisterScenesFromDirectory(
        m_sceneManager, projectRoot / L"Assets" / L"Scenes", resources);
}

void ProjectRuntime::BindExternalScene(Scene* scene)
{
    m_sceneManager.SetScene(scene);
}

void ProjectRuntime::ReleaseOwnedScene()
{
    m_sceneManager.ReleaseOwnedScene();
}

void ProjectRuntime::LoadScene(const std::string& sceneName)
{
    m_sceneManager.LoadScene(sceneName);
}

void ProjectRuntime::LoadScene(const std::filesystem::path& sceneFile)
{
    LoadScene(util::StringUtils::PathToUtf8(sceneFile.stem()));
}

void ProjectRuntime::Update(float dt,
                            const ProjectSettings& settings,
                            bool simulating,
                            bool singleStep,
                            bool playing)
{
    ApplyDisplayIntents();
    /// @note Module ごとの差異を許さず、Physics 設定 → Simulation 状態 → Scheduler の順に固定する。
    ApplyPhysicsSettings(m_physicsWorld, settings);
    m_sceneManager.SetPhysicsHz(settings.physics.hz);
    m_sceneManager.SetSimulating(simulating);
    m_sceneManager.SetPlaying(playing);
    m_sceneManager.SetSingleStep(singleStep);
    Script::SetPhysicsWorld(simulating ? &m_physicsWorld : nullptr);
    m_sceneManager.Update(dt, m_physicsWorld);
}

void ProjectRuntime::LateUpdate(float dt)
{
    m_sceneManager.LateUpdate(dt, m_physicsWorld);
}

void ProjectRuntime::ResetPhysics(const ProjectSettings& settings)
{
    m_physicsWorld = physics::World{};
    ApplyPhysicsSettings(m_physicsWorld, settings);
}

void ProjectRuntime::ActivateScriptRuntime(renderer::IRenderer& renderer,
                                           uint32_t viewportWidth,
                                           uint32_t viewportHeight)
{
    m_scriptRuntime = ScriptRuntime{
        &m_sceneManager,
        &renderer,
        viewportWidth,
        viewportHeight,
        m_scriptRuntime.requestQuit,
        m_scriptRuntime.quitContext
    };
    ScriptRuntime::Override(&m_scriptRuntime);
    BindDisplayHandler();
}

void ProjectRuntime::UpdateScriptViewport(uint32_t viewportWidth, uint32_t viewportHeight)
{
    m_scriptRuntime.viewportWidth = viewportWidth;
    m_scriptRuntime.viewportHeight = viewportHeight;
}

void ProjectRuntime::ActivateScriptRuntime(uint32_t viewportWidth, uint32_t viewportHeight)
{
    m_scriptRuntime.sceneManager = &m_sceneManager;
    m_scriptRuntime.renderer = nullptr;
    m_scriptRuntime.viewportWidth = viewportWidth;
    m_scriptRuntime.viewportHeight = viewportHeight;
    ScriptRuntime::Override(&m_scriptRuntime);
    BindDisplayHandler();
}
void ProjectRuntime::BindDisplayHandler()
{
    m_scriptRuntime.displayContext = this;
    m_scriptRuntime.requestDisplay = [](void* context, uint64_t lifetime, uint32_t operation, uint32_t first, uint32_t second) {
        auto& owner = *static_cast<ProjectRuntime*>(context); auto& app = core::Application::Get();
        const Scene* scene = owner.GetActiveScene();
        if (operation >= owner.m_displayIntents.size() || !scene || scene->GetRenderSceneGeneration() != lifetime
            || app.IsEditorHosted() || !app.GetWindowWidth() || !app.GetWindowHeight()) return false;
        if ((operation == 0 || operation == 2) && first > 1) return false;
        if (operation == 1 && (!first || !second || first > INT32_MAX || second > INT32_MAX)) return false;
        owner.m_displayIntents[operation] = {lifetime, first, second, true}; return true;
    };
}
void ProjectRuntime::ApplyDisplayIntents()
{
    const Scene* scene = GetActiveScene(); auto& app = core::Application::Get();
    for (uint32_t operation = 0; operation < m_displayIntents.size(); ++operation) {
        const auto intent = m_displayIntents[operation]; m_displayIntents[operation].pending = false;
        if (!intent.pending || !scene || !Script::IsInPlayMode() || scene->GetRenderSceneGeneration() != intent.scene || app.IsEditorHosted()
            || !app.GetWindowWidth() || !app.GetWindowHeight()) continue;
        if (operation == 0) app.GetWindow().SetWindowMode(intent.first ? core::WindowMode::BorderlessFullscreen : core::WindowMode::Windowed);
        else if (operation == 1) app.GetWindow().SetClientSize(intent.first, intent.second);
        else if (m_scriptRuntime.renderer) m_scriptRuntime.renderer->SetVSync(intent.first != 0);
    }
}

void ProjectRuntime::SetScriptQuitHandler(ScriptRuntime::QuitHandler handler, void* context)
{
    m_scriptRuntime.requestQuit = handler;
    m_scriptRuntime.quitContext = handler ? context : nullptr;
}

void ProjectRuntime::Shutdown()
{
    /// @note Play 中の LoadScene 後に Editor 外部 Scene へ戻ると、SceneManager には外部 Scene と
    /// @note 遷移時の owned Scene が同時に存在する。CurrentScene だけを Clear すると owned Scene の
    /// @note DLL 由来 Script が残るため、FreeLibrary 前に両方を明示的に破棄する。
    m_sceneManager.ClearScenes();
    /// @note OnDestroy can request quit while Play is still active; keep the host hook and override until every teardown callback has returned.
    SetScriptQuitHandler(nullptr, nullptr);
    m_scriptRuntime.requestDisplay = nullptr; m_scriptRuntime.displayContext = nullptr; m_displayIntents = {};
    Script::SetPhysicsWorld(nullptr);
    if (ScriptRuntime::GetOverride() == &m_scriptRuntime) {
        ScriptRuntime::Override(nullptr);
    }
    m_sceneManager.SetSimulating(false);
    m_sceneManager.SetPlaying(false);

    /// @note m_gameUICtx は Play セッションごとに作り直される。抱えている頂点バッファは
    /// @note ResourceHandle なのでデストラクタでは返せず、ここで返さないと往復のたびに残る。
    if (renderer::ResourceManager* resources = renderer::ResourceManager::Active())
        UISystemReleaseGpuResources(m_gameUICtx, *resources);
}

} // namespace fbzz::scene
