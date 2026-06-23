// FBZZ Engine
// ProjectRuntime.cpp | fbzz::scene
// Editor PlayとStandaloneで共有するプロジェクト実行パイプライン実装
#include <Engine/Scene/ProjectRuntime.hpp>

#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Scene/ProjectLauncher.hpp>
#include <Engine/Scene/SceneUtils.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Util/StringUtils.hpp>

namespace fbzz::scene {

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
                            bool singleStep)
{
    // WHAT: Moduleごとの差異を許さず、Physics設定→Simulation状態→Schedulerの順に固定する。
    ApplyPhysicsSettings(m_physicsWorld, settings);
    m_sceneManager.SetPhysicsHz(settings.physics.hz);
    m_sceneManager.SetSimulating(simulating);
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
        viewportHeight
    };
    ScriptRuntime::Override(&m_scriptRuntime);
}

void ProjectRuntime::UpdateScriptViewport(uint32_t viewportWidth, uint32_t viewportHeight)
{
    m_scriptRuntime.viewportWidth = viewportWidth;
    m_scriptRuntime.viewportHeight = viewportHeight;
}

void ProjectRuntime::Shutdown()
{
    // WHY: Play中のLoadScene後にEditor外部Sceneへ戻ると、SceneManagerには外部Sceneと
    //      遷移時のowned Sceneが同時に存在する。CurrentSceneだけをClearするとowned Sceneの
    //      DLL由来Scriptが残るため、FreeLibrary前に両方を明示的に破棄する。
    m_sceneManager.ClearScenes();
    Script::SetPhysicsWorld(nullptr);
    if (ScriptRuntime::GetOverride() == &m_scriptRuntime) {
        ScriptRuntime::Override(nullptr);
    }
    m_sceneManager.SetSimulating(false);
}

} // namespace fbzz::scene
