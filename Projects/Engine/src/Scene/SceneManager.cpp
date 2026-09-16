/// @file    SceneManager.cpp
/// @brief   Scene 遷移と System 更新順の管理。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// 登録済み Scene をアクティブ化し、フレーム境界で LoadScene を適用する。
/// RenderSystem は BeginFrame / EndFrame の都合でゲームループ側から呼ぶ。
#include "Engine/Scene/SceneManager.hpp"
#include "Engine/Core/Cursor.hpp"
#include "Engine/Scene/PrefabPool.hpp"
#include "Engine/Scene/SceneSerializer.hpp"
#include "Engine/Scene/Systems/TransformSystem.hpp"
#include "Engine/Scene/Systems/NavMeshBakeSystem.hpp"
#include "Engine/Scene/Systems/NavMeshSensorSystem.hpp"
#include "Engine/Scene/Systems/NavMeshPatrolSystem.hpp"
#include "Engine/Scene/Systems/BehaviorTreeSystem.hpp"
#include "Engine/Scene/Systems/NavigationSystem.hpp"
#include "Engine/Scene/Systems/PhysicsSystem.hpp"
#include "Engine/Scene/Systems/ScriptSystem.hpp"
#include "Engine/Scene/Systems/AnimatorSystem.hpp"
#include "Engine/Scene/Systems/IKSystem.hpp"
#include "Engine/Scene/Systems/RagdollSystem.hpp"
#include "Engine/Scene/Systems/SpringBoneSystem.hpp"
#include "Engine/Scene/Systems/LifetimeSystem.hpp"
#include "Engine/Scene/Systems/AudioSystem.hpp"
#include "Engine/Scene/Systems/LODSystem.hpp"
#include "Engine/Scene/Systems/UIAnimatorSystem.hpp"
#include "Engine/Scene/Systems/UIAudioSystem.hpp"
#include "Engine/Scene/Systems/ParticleSimulationSystem.hpp"
#include "Engine/Scene/Systems/WeatherSystem.hpp"
#include "Engine/Scene/Systems/WaterSystem.hpp"
#include "Engine/Scene/Systems/SequenceSystem.hpp"
#include "Engine/Scene/Systems/VFXSystem.hpp"
#include "Engine/Scene/Systems/VFXBeamSystem.hpp"
#include "Engine/Scene/Systems/VFXLineSystem.hpp"
#include "Engine/Scene/Systems/LightFlickerSystem.hpp"
#include "Engine/Scene/Systems/GameplayComponentSystems.hpp"
#include "Engine/Scene/Systems/RuntimeMeshSystem.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include <Engine/Core/Logger.hpp>
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
    m_scheduler.AddSystem<NavMeshBakeSystem>();

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
    m_scheduler.AddSystem<WaterSystem>();

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
    // .sequence は VFX / Audio へ書き込むため、両者より先に評価する
    // (実行順は SequenceSystem の OrderingHints が決める)。
    m_scheduler.AddSystem<SequenceSystem>();
    m_scheduler.AddSystem<VFXSystem>();
    m_scheduler.AddSystem<VFXBeamSystem>();
    m_scheduler.AddSystem<VFXLineSystem>();
    // 明滅は VFXLightEnvelope と同じ intensity を書くので、VFX の後に置く
    // (実行順は LightFlickerSystem の OrderingHints が決める)。
    m_scheduler.AddSystem<LightFlickerSystem>();
    // UI の効果音は AudioSystem より前に積む (OrderingHints が実際の順を決める)。
    m_scheduler.AddSystem<UIAudioSystem>();
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
    // 揺れものは IK が確定した姿勢を静止姿勢として読むので、必ず IK より後。
    m_scheduler.AddSystem<SpringBoneSystem>();
    // ラグドールは «倒れる数秒» だけ骨を丸ごと持っていく。揺れものより後に置き、
    // 戻り際は揺れ込みの姿勢へブレンドする。
    m_scheduler.AddSystem<RagdollSystem>();
    // DCC Bone姿勢をAnimator/IKが確定した後にSocket、Camera、表示補助を評価する。
    m_scheduler.AddSystem<ConstraintSystem>();
    m_scheduler.AddSystem<SplineSystem>();
    m_scheduler.AddSystem<CameraRigSystem>();
    m_scheduler.AddSystem<BillboardSystem>();
    m_scheduler.AddSystem<PresentationSystem>();
    // Script が組んだメッシュを GPU へ載せる。Sprite / Line と同じ «CPU で作って
    // LateUpdate で焼く» 経路なので、PresentationSystem の隣に置く。
    m_scheduler.AddSystem<RuntimeMeshSystem>();
    // 雨量はパーティクルの発生量を決めるので、シミュレーションより先 (OrderingHints で明示)。
    m_scheduler.AddSystem<WeatherSystem>();
    m_scheduler.AddSystem<ParticleSimulationSystem>();

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

bool SceneManager::LoadScene(const std::string& name)
{
    // WHY assert しないか: 遷移先の名前はシーンやスクリプトが持つデータで、
    //     綴り間違いも「まだ作っていないシーン」もありうる。assert だと
    //     UI ボタンを 1 回押しただけでエディタが abort する。
    //     要求を拒否して false を返し、呼び出し側が演出を巻き戻せるようにする。
    if (!m_factories.count(name)) {
        FBZZ_LOG_ERROR("SceneManager: scene '%s' is not registered. "
                       "Check the name and that Assets/Scenes contains it.", name.c_str());
        return false;
    }
    m_pendingLoad = name;
    return true;
}

void SceneManager::SetScene(Scene* scene)
{
    m_externalScene = scene;
    // 外部 Scene を差し直した時点で、遷移で切り替えた名前は実行対象ではなくなる。
    if (scene) m_activeName.clear();
}

void SceneManager::ReleaseOwnedScene()
{
    // 外部 Scene と同一実体になることはないが、なった場合に所有していない側を消さない。
    if (m_active && m_active.get() != m_externalScene) {
        // プールの待機列は EntityID を保持するため、Scene を空にする前に捨てる。
        PrefabPool::Clear(*m_active);
        m_active->Clear();
        m_active.reset();
    }
    m_pendingLoad.clear();
    m_activeName.clear();
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
    m_activeName.clear();
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

void SceneManager::SetPlaying(bool playing)
{
    m_playing = playing;
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
        if (factoryIt == m_factories.end()) {
            FBZZ_LOG_ERROR("SceneManager: pending scene '%s' is no longer registered.",
                           m_pendingLoad.c_str());
            m_pendingLoad.clear();
            return;
        }

        // ロードに失敗しても現在の Scene は保持したまま続ける。ここで実行対象を
        // 失うと、遷移できなかっただけのはずが「以降なにも動かない」に化ける。
        std::unique_ptr<Scene> nextScene = factoryIt->second();
        if (!nextScene) {
            FBZZ_LOG_ERROR("SceneManager: failed to load scene '%s'. Staying in the current scene.",
                           m_pendingLoad.c_str());
            m_pendingLoad.clear();
            return;
        }

        // WHY ここでカーソルを畳むか: 要求はスクリプトの寿命に紐づくので、古い Scene を
        //     捨てた時点でほとんどは自動的に外れる。残るのは cursor.SetLockMode で
        //     基底を直接書いた分で、これは «誰も外さない» まま次の画面へ持ち越される。
        //     画面をまたいで引き継いでよいカーソル状態は無い、と決めておく。
        //     新しい Scene のスクリプトはこの直後の Update で名乗り直すため、
        //     プレイヤーから見て既定へ戻る瞬間は現れない。
        // Scene のデストラクタはコールバックを呼ばない。次の OnStart が
        // 破棄済みのシングルトンを読む前に、旧シーンの登録と設定を片付ける。
        if (m_active) m_active->Clear();
        core::Cursor::ClearRequests();

        // Play中のPreScriptではTransformEditorPreviewが動かない。
        // 初回スクリプトが未計算の原点を接触・出現位置として使う前に階層を確定する。
        FlushWorldTransforms(*nextScene);
        m_active = std::move(nextScene);
        // 前シーンの固定ステップの残時間を新しい盤面へ持ち込まない。
        m_scheduler.ResetAccumulator();
        // LoadScene は明示的な遷移要求なので、新しい owned Scene を外部 Scene より優先する。
        m_externalScene = nullptr;
        m_activeName    = m_pendingLoad;
        m_pendingLoad.clear();
    }

    Scene* scene = CurrentScene();
    if (!scene) return;

    m_scheduler.Update({ *scene, world, nullptr, m_audioManager, dt, 0.0f, m_simulating, m_playing });
}

void SceneManager::LateUpdate(float dt, physics::World& world)
{
    FBZZ_PROFILE_SCOPE("SceneManager::LateUpdate");

    Scene* scene = CurrentScene();
    if (!scene) return;

    m_scheduler.LateUpdate({ *scene, world, renderer::ResourceManager::Active(), m_audioManager, dt, 0.0f, m_simulating, m_playing });
}

Scene* SceneManager::GetActive()
{
    // Editor の外部 Scene と Standalone の owned Scene を同じ取得 API で扱う。
    return CurrentScene();
}

} // namespace fbzz::scene
