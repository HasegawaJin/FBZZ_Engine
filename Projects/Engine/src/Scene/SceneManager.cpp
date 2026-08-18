// FBZZ Engine
// SceneManager.cpp | fbzz::scene
// Scene 遷移と System 更新順の管理
// 登録済み Scene をアクティブ化し、フレーム境界で LoadScene を適用する。
// RenderSystem は BeginFrame / EndFrame の都合でゲームループ側から呼ぶ。
#include "Engine/Scene/SceneManager.hpp"
#include "Engine/Scene/PrefabPool.hpp"
#include "Engine/Scene/SceneSerializer.hpp"
#include "Engine/Scene/Systems/TransformSystem.hpp"
#include "Engine/Scene/Systems/FoliageBakeSystem.hpp"
#include "Engine/Scene/Systems/FoliageCullSystem.hpp"
#include "Engine/Scene/Systems/NavMeshBakeSystem.hpp"
#include "Engine/Scene/Systems/NavMeshSensorSystem.hpp"
#include "Engine/Scene/Systems/NavMeshPatrolSystem.hpp"
#include "Engine/Scene/Systems/BehaviorTreeSystem.hpp"
#include "Engine/Scene/Systems/NavigationSystem.hpp"
#include "Engine/Scene/Systems/PhysicsSystem.hpp"
#include "Engine/Scene/Systems/ScriptSystem.hpp"
#include "Engine/Scene/Systems/AnimatorSystem.hpp"
#include "Engine/Scene/Systems/IKSystem.hpp"
#include "Engine/Scene/Systems/LifetimeSystem.hpp"
#include "Engine/Scene/Systems/AudioSystem.hpp"
#include "Engine/Scene/Systems/LODSystem.hpp"
#include "Engine/Scene/Systems/UIAnimatorSystem.hpp"
#include "Engine/Scene/Systems/ParticleSimulationSystem.hpp"
#include "Engine/Scene/Systems/VFXGraphSystem.hpp"
#include "Engine/Scene/Systems/GameplayComponentSystems.hpp"
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

    // Physics (固定ステップ)
    // FixedScriptSystem は OrderingHints で PhysicsSystem より前に並ぶ。
    m_scheduler.AddSystem<FixedScriptSystem>();
    m_scheduler.AddSystem<PhysicsSystem>();

    // PostPhysics
    m_scheduler.AddSystem<TransformPostPhysics>();
    m_scheduler.AddSystem<TransformPresentationPostPhysics>();

    // Navigation
    // 実行順は各 System の OrderingHints が決める:
    //   Sensor → BehaviorTree → Patrol → Navigation
    // BehaviorTree を Patrol より前に置くのは、BT が SetTarget を呼んだ同じフレームで
    // NavMeshPatrolSystem の `agent->target.IsValid()` による巡回抑止を効かせるため。
    m_scheduler.AddSystem<NavMeshSensorSystem>();
    m_scheduler.AddSystem<BehaviorTreeSystem>();
    m_scheduler.AddSystem<NavMeshPatrolSystem>();
    m_scheduler.AddSystem<NavigationSystem>();

    // LateScript
    m_scheduler.AddSystem<LateScriptSystem>();
    m_scheduler.AddSystem<VFXGraphSystem>();
    m_scheduler.AddSystem<AudioSystem>();

    // Cleanup
    m_scheduler.AddSystem<LifetimeSystem>();
    m_scheduler.AddSystem<FlushDestroyQueueSystem>();

    // LateUpdate
    m_scheduler.AddSystem<UIAnimatorSystem>();
    m_scheduler.AddSystem<TransformLateUpdate>();
    m_scheduler.AddSystem<LODSystem>();
    m_scheduler.AddSystem<AnimatorSystem>();
    m_scheduler.AddSystem<IKSystem>();
    // DCC Bone姿勢をAnimator/IKが確定した後にSocket、Camera、表示補助を評価する。
    m_scheduler.AddSystem<ConstraintSystem>();
    m_scheduler.AddSystem<SplineSystem>();
    m_scheduler.AddSystem<CameraRigSystem>();
    m_scheduler.AddSystem<BillboardSystem>();
    m_scheduler.AddSystem<PresentationSystem>();
    m_scheduler.AddSystem<ParticleSimulationSystem>();
    m_scheduler.AddSystem<TransformPresentationLateUpdate>();

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

void SceneManager::ClearScenes()
{
    // 外部Sceneとowned Sceneは通常別実体だが、同じ実体を二重にClearしないよう比較する。
    Scene* const external = m_externalScene;
    if (external) {
        // プールの待機列は EntityID を保持するため、Scene を空にする前に捨てる。
        PrefabPool::Clear(*external);
        external->Clear();
    }
    if (m_active && m_active.get() != external) {
        PrefabPool::Clear(*m_active);
        m_active->Clear();
    }

    // WHAT: owned Scene自体もDLLロード中に破棄し、仮想デストラクタの呼び残しを防ぐ。
    m_active.reset();
    m_externalScene = nullptr;
    m_pendingLoad.clear();
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
        // Factory 完了までは現在の Scene を保持し、ロード失敗で実行対象を失わないようにする。
        auto factoryIt = m_factories.find(m_pendingLoad);
        assert(factoryIt != m_factories.end() && "Pending scene is not registered");
        if (factoryIt == m_factories.end()) {
            m_pendingLoad.clear();
            return;
        }

        std::unique_ptr<Scene> nextScene = factoryIt->second();
        if (!nextScene) {
            m_pendingLoad.clear();
            return;
        }

        m_active = std::move(nextScene);
        // LoadScene は明示的な遷移要求なので、新しい owned Scene を外部 Scene より優先する。
        m_externalScene = nullptr;
        m_pendingLoad.clear();
    }

    Scene* scene = CurrentScene();
    if (!scene) return;

    m_scheduler.Update({ *scene, world, nullptr, m_audioManager, dt, 0.0f, 0.0f, m_simulating });
}

void SceneManager::LateUpdate(float dt, physics::World& world)
{
    FBZZ_PROFILE_SCOPE("SceneManager::LateUpdate");

    Scene* scene = CurrentScene();
    if (!scene) return;

    m_scheduler.LateUpdate({ *scene, world, renderer::ResourceManager::Active(), m_audioManager, dt, 0.0f, 0.0f, m_simulating });
}

Scene* SceneManager::GetActive()
{
    // Editor の外部 Scene と Standalone の owned Scene を同じ取得 API で扱う。
    return CurrentScene();
}

} // namespace fbzz::scene
