// FBZZ Engine
// SceneManager.cpp | fbzz::scene
// シーン遷移と System 実行順序の管理
#include "engine/Scene/SceneManager.hpp"
#include "engine/Scene/Systems/TransformSystem.hpp"
#include "engine/Scene/Systems/RenderSystem.hpp"
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

void SceneManager::Update(float dt,
                          renderer::IRenderer& renderer,
                          physics::World& world) {
    // フレーム先頭: ペンディングのシーン切り替えを適用
    if (!m_pendingLoad.empty()) {
        m_active      = m_factories[m_pendingLoad]();
        m_pendingLoad.clear();
    }

    if (!m_active) return;

    // System 実行順序 (変更する場合はここだけ触る)
    PhysicsSystem(*m_active, world, dt);    // 1. 物理を先に解決
    TransformSystem(*m_active);             // 2. ワールド行列を更新
    // RenderSystem は Renderer 追加 (Step 5-3) 後に有効化する
    // RenderSystem(*m_active, renderer, camera, lights);

    m_active->FlushDestroyQueue(dt);        // フレーム末尾で削除
}

Scene* SceneManager::GetActive() {
    return m_active.get();
}

} // namespace fbzz::scene
