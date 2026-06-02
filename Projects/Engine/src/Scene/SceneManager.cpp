// FBZZ Engine
// SceneManager.cpp | fbzz::scene
// Scene 遷移と System 更新順の管理
// 登録済み Scene をアクティブ化し、フレーム境界で LoadScene を適用する。
// RenderSystem は BeginFrame / EndFrame の都合でゲームループ側から呼ぶ。
#include "Engine/Scene/SceneManager.hpp"
#include "Engine/Scene/SceneSerializer.hpp"
#include "Engine/Scene/Systems/TransformSystem.hpp"
#include "Engine/Scene/Systems/PhysicsSystem.hpp"
#include "Engine/Scene/Systems/ScriptSystem.hpp"
#include "Engine/Scene/Systems/AnimatorSystem.hpp"
#include "Engine/Scene/Systems/IKSystem.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include <Engine/Profiler/ProfileScope.hpp>
#include <cassert>

namespace fbzz::scene {

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

void SceneManager::Update(float dt, physics::World& world)
{
    FBZZ_PROFILE_SCOPE("SceneManager::Update");

    if (!m_pendingLoad.empty()) {
        FBZZ_PROFILE_SCOPE("SceneManager::LoadPendingScene");
        m_active = m_factories[m_pendingLoad]();
        m_pendingLoad.clear();
    }

    if (!m_active) return;

    {
        FBZZ_PROFILE_SCOPE("TransformSystem");
        TransformSystem(*m_active);
    }
    PhysicsSystem(*m_active, world, dt);
    if (auto* resources = renderer::ResourceManager::Active()) {
        AnimatorSystem(*m_active, *resources, dt);
        // AnimatorSystem が FK ポーズとスキニング行列を作った直後に IK を適用する。
        // WHY: IK はアニメーション結果を補正する後段処理なので、先に呼ぶと AnimatorSystem に上書きされる。
        IKSystem(*m_active, world, *resources, dt);
    }
    ScriptSystem(*m_active, dt);
    {
        FBZZ_PROFILE_SCOPE("Scene::FlushDestroyQueue");
        m_active->FlushDestroyQueue(dt);
    }
}

Scene* SceneManager::GetActive()
{
    return m_active.get();
}

} // namespace fbzz::scene
