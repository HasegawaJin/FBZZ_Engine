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
#include "Engine/Scene/Systems/PhysicsSystem.hpp"
#include "Engine/Scene/Systems/ScriptSystem.hpp"
#include "Engine/Scene/Systems/AnimatorSystem.hpp"
#include "Engine/Scene/Systems/IKSystem.hpp"
#include "Engine/Scene/Systems/LifetimeSystem.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include <Engine/Profiler/ProfileScope.hpp>
#include <string>
#include <cassert>

namespace fbzz::scene {

namespace {
const char* PhysicsStepCountMarkerName(int steps)
{
    // WHY: 固定タイムステップの catch-up で PhysicsSystem が 1 フレームに複数回走ることがある。
    //      Profiler marker として回数を残し、重複呼び出し疑いを確認しやすくする。
    std::string buf = "PhysicsFixedSteps=" + std::to_string(steps);

    return buf.c_str();
}
} // namespace

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
    m_physicsHz = hz < 1 ? 1 : hz;
}

void SceneManager::SetSimulating(bool simulating)
{
    m_simulating = simulating;
    if (!simulating)
        m_physicsAccumulator = 0.0f;
}

void SceneManager::SetSingleStep(bool singleStep)
{
    m_singleStep = singleStep;
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

    if (!m_simulating) {
        // ── 停止中: Transform のみ (エディタ上のオブジェクト移動を反映する) ────
        FBZZ_PROFILE_SCOPE("SceneManager::TransformSystem");
        TransformSystem(*scene);
        FoliageBakeSystem(*scene);
        FoliageCullSystem(*scene);
        return;
    }

    // ── Phase 1: Script Update ──────────────────────────────────────────────
    { FBZZ_PROFILE_SCOPE("SceneManager::ScriptSystem");
      ScriptSystem(*scene, dt); }

    // ── Phase 2: Transform ──────────────────────────────────────────────────
    { FBZZ_PROFILE_SCOPE("SceneManager::TransformSystem");
      TransformSystem(*scene); }

    // ── Phase 2.5: Foliage Bake / Cull (Physics より前に GO 生成と距離カリングを完了) ─
    { FBZZ_PROFILE_SCOPE("SceneManager::FoliageBakeSystem");
      FoliageBakeSystem(*scene); }
    { FBZZ_PROFILE_SCOPE("SceneManager::FoliageCullSystem");
      FoliageCullSystem(*scene); }

    // ── Phase 3: Physics (fixed timestep) ───────────────────────────────────
    {
        FBZZ_PROFILE_SCOPE("SceneManager::PhysicsFixedStepLoop");
        const float fixedDt = 1.0f / static_cast<float>(m_physicsHz);
        int steps = 0;
        if (m_singleStep) {
            PhysicsSystem(*scene, world, fixedDt);
            steps = 1;
        } else {
            m_physicsAccumulator += dt;
            const float maxAccum = fixedDt * 8.0f;
            if (m_physicsAccumulator > maxAccum) m_physicsAccumulator = maxAccum;
            while (m_physicsAccumulator >= fixedDt) {
                PhysicsSystem(*scene, world, fixedDt);
                m_physicsAccumulator -= fixedDt;
                ++steps;
            }
        }
        if (steps >= 2) FBZZ_PROFILE_MARKER(PhysicsStepCountMarkerName(steps));
    }

    // ── Phase 4: Transform (physics writeback) ──────────────────────────────
    { FBZZ_PROFILE_SCOPE("SceneManager::TransformSystem");
      TransformSystem(*scene); }

    // ── Phase 5: Script LateUpdate ──────────────────────────────────────────
    { FBZZ_PROFILE_SCOPE("SceneManager::LateScriptSystem");
      LateScriptSystem(*scene, dt); }

    // ── Phase 6: Lifetime & Cleanup ─────────────────────────────────────────
    { FBZZ_PROFILE_SCOPE("SceneManager::LifetimeSystem");
      LifetimeSystem(*scene, dt); }
    { FBZZ_PROFILE_SCOPE("SceneManager::FlushDestroyQueue");
      scene->FlushDestroyQueue(dt); }
}

void SceneManager::LateUpdate(float dt, physics::World& world)
{
    FBZZ_PROFILE_SCOPE("SceneManager::LateUpdate");

    Scene* scene = CurrentScene();
    if (!scene) return;

    // ── Phase 7: Transform (FlushDestroyQueue 後のリフレッシュ) ─────────────
    { FBZZ_PROFILE_SCOPE("SceneManager::TransformSystem");
      TransformSystem(*scene); }

    // ── Phase 8: Animation ──────────────────────────────────────────────────
    // AnimatorSystem が FK ポーズとスキニング行列を作った直後に IK を適用する。
    // WHY: IK はアニメーション結果を補正する後段処理なので、先に呼ぶと AnimatorSystem に上書きされる。
    if (auto* resources = renderer::ResourceManager::Active()) {
        { FBZZ_PROFILE_SCOPE("SceneManager::AnimatorSystem");
          AnimatorSystem(*scene, *resources, dt); }
        { FBZZ_PROFILE_SCOPE("SceneManager::IKSystem");
          IKSystem(*scene, world, *resources, dt); }
    }
}

Scene* SceneManager::GetActive()
{
    return m_active.get();
}

} // namespace fbzz::scene
