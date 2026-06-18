// FBZZ Engine
// SceneManager.cpp | fbzz::scene
// Scene 遷移と System 更新順の管理
// 登録済み Scene をアクティブ化し、フレーム境界で LoadScene を適用する。
// RenderSystem は BeginFrame / EndFrame の都合でゲームループ側から呼ぶ。
#include "Engine/Scene/SceneManager.hpp"
#include "Engine/Scene/SceneSerializer.hpp"
#include "Engine/Scene/Systems/TransformSystem.hpp"
#include "Engine/Scene/Systems/FoliageBakeSystem.hpp"
#include "Engine/Scene/Systems/FoliageCullSystem.hpp"
#include "Engine/Scene/Systems/NavMeshBakeSystem.hpp"
#include "Engine/Scene/Systems/NavMeshSensorSystem.hpp"
#include "Engine/Scene/Systems/NavMeshPatrolSystem.hpp"
#include "Engine/Scene/Systems/NavigationSystem.hpp"
#include "Engine/Scene/Systems/PhysicsSystem.hpp"
#include "Engine/Scene/Systems/ScriptSystem.hpp"
#include "Engine/Scene/Systems/AnimatorSystem.hpp"
#include "Engine/Scene/Systems/FootIKSystem.hpp"
#include "Engine/Scene/Systems/IKSystem.hpp"
#include "Engine/Scene/Systems/LifetimeSystem.hpp"
#include "Engine/Scene/Systems/AudioSystem.hpp"
#include "Engine/Scene/Systems/UIAnimatorSystem.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include <Engine/Profiler/ProfileScope.hpp>
#include <string>
#include <cassert>

namespace fbzz::scene {

SceneManager::SceneManager()
{
    BuildScheduler();
}

void SceneManager::BuildScheduler()
{
    // Physics 固定ステップを設定
    m_scheduler.ConfigurePhase(Phase::Physics, { .fixedStep = true, .hz = 60, .maxCatchUp = 8.0f });

    // PreScript
    m_scheduler.AddSystem<TransformEditorPreview>();
    m_scheduler.AddSystem<FoliageBakeSystem>();
    m_scheduler.AddSystem<NavMeshBakeSystem>();
    m_scheduler.AddSystem<FoliageCullSystem>();

    // Script
    m_scheduler.AddSystem<ScriptSystem>();

    // PrePhysics
    m_scheduler.AddSystem<TransformPrePhysics>();

    // Physics
    m_scheduler.AddSystem<PhysicsSystem>();

    // PostPhysics
    m_scheduler.AddSystem<TransformPostPhysics>();

    // Navigation
    m_scheduler.AddSystem<NavMeshSensorSystem>();
    m_scheduler.AddSystem<NavMeshPatrolSystem>();
    m_scheduler.AddSystem<NavigationSystem>();

    // LateScript
    m_scheduler.AddSystem<LateScriptSystem>();
    m_scheduler.AddSystem<AudioSystem>();

    // Cleanup
    m_scheduler.AddSystem<LifetimeSystem>();
    m_scheduler.AddSystem<FlushDestroyQueueSystem>();

    // LateUpdate
    m_scheduler.AddSystem<UIAnimatorSystem>();
    m_scheduler.AddSystem<TransformLateUpdate>();
    m_scheduler.AddSystem<AnimatorSystem>();
    m_scheduler.AddSystem<FootIKSystem>();
    m_scheduler.AddSystem<IKSystem>();

    m_scheduler.Build();
}

void SceneManager::Register(const std::string& name, SceneFactory factory)
{
    m_factories[name] = std::move(factory);
}

void SceneManager::RegisterFromFile(const std::string& name, const std::string& path,
                                    renderer::ResourceManager& resources)
{
    Register(name, [path, &resources]() {
        return SceneSerializer::Load(path, resources);
    });
}

void SceneManager::LoadScene(const std::string& name)
{
    assert(m_factories.count(name) && "Scene is not registered");
    m_pendingLoad = name;
}

void SceneManager::SetScene(Scene* scene)
{
    m_externalScene = scene;
}

void SceneManager::SetPhysicsHz(int hz)
{
    hz = hz < 1 ? 1 : hz;
    m_scheduler.ConfigurePhase(Phase::Physics, { .fixedStep = true, .hz = hz, .maxCatchUp = 8.0f });
}

void SceneManager::SetSimulating(bool simulating)
{
    m_simulating = simulating;
    if (!simulating)
        m_scheduler.ResetAccumulator();
}

void SceneManager::SetAudioManager(audio::AudioManager* audioManager)
{
    m_audioManager = audioManager;
}

void SceneManager::SetSingleStep(bool singleStep)
{
    m_scheduler.SetSingleStep(singleStep);
}

Scene* SceneManager::CurrentScene() const
{
    return m_externalScene ? m_externalScene : m_active.get();
}

void SceneManager::Update(float dt, physics::World& world)
{
    FBZZ_PROFILE_SCOPE("SceneManager::Update");

    if (!m_pendingLoad.empty()) {
        FBZZ_PROFILE_SCOPE("SceneManager::LoadPendingScene");
        m_active = m_factories[m_pendingLoad]();
        m_pendingLoad.clear();
    }

    Scene* scene = CurrentScene();
    if (!scene) return;

    m_scheduler.Update({ *scene, world, nullptr, m_audioManager, dt, 0.0f, m_simulating });
}

void SceneManager::LateUpdate(float dt, physics::World& world)
{
    FBZZ_PROFILE_SCOPE("SceneManager::LateUpdate");

    Scene* scene = CurrentScene();
    if (!scene) return;

    m_scheduler.LateUpdate({ *scene, world, renderer::ResourceManager::Active(), m_audioManager, dt, 0.0f, m_simulating });
}

Scene* SceneManager::GetActive()
{
    return m_active.get();
}

} // namespace fbzz::scene
