// FBZZ Engine
// SceneManager.cpp | fbzz::scene
// Scene transition and system update order management
#include "engine/Scene/SceneManager.hpp"
#include "engine/Scene/Systems/TransformSystem.hpp"
#include "engine/Scene/Systems/PhysicsSystem.hpp"
#include "engine/Scene/Systems/ScriptSystem.hpp"
#include <cassert>

namespace fbzz::scene {

void SceneManager::Register(const std::string& name, SceneFactory factory)
{
    m_factories[name] = std::move(factory);
}

void SceneManager::LoadScene(const std::string& name)
{
    assert(m_factories.count(name) && "Scene is not registered");
    m_pendingLoad = name;
}

void SceneManager::Update(float dt, physics::World& world)
{
    if (!m_pendingLoad.empty()) {
        m_active = m_factories[m_pendingLoad]();
        m_pendingLoad.clear();
    }

    if (!m_active) return;

    TransformSystem(*m_active);
    PhysicsSystem(*m_active, world, dt);
    ScriptSystem(*m_active, dt);
    m_active->FlushDestroyQueue(dt);
}

Scene* SceneManager::GetActive()
{
    return m_active.get();
}

} // namespace fbzz::scene
