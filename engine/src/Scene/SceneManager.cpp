// FBZZ Engine
// SceneManager.cpp | fbzz::scene
// シーン遷移と System 実行順序の管理
#include "engine/Scene/SceneManager.hpp"
#include "engine/Scene/Systems/TransformSystem.hpp"
#include "engine/Scene/Systems/PhysicsSystem.hpp"
#include <cassert>

namespace fbzz::scene {

void SceneManager::Register(const std::string& name, SceneFactory factory) {
    m_factories[name] = std::move(factory);
}

void SceneManager::LoadScene(const std::string& name) {
    assert(m_factories.count(name) && "未登録のシーン名です");
    m_pendingLoad = name;
}

void SceneManager::Update(float dt, physics::World& world) {
    if (!m_pendingLoad.empty()) {
        m_active      = m_factories[m_pendingLoad]();
        m_pendingLoad.clear();
    }

    if (!m_active) return;

    TransformSystem(*m_active);             // 1. 階層を解決 (local → world)
    PhysicsSystem(*m_active, world, dt);    // 2. 物理が world 座標を上書き
    m_active->FlushDestroyQueue(dt);        // 3. フレーム末尾で削除
}

Scene* SceneManager::GetActive() {
    return m_active.get();
}

} // namespace fbzz::scene
