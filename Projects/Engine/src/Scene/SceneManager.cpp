// FBZZ Engine
// SceneManager.cpp | fbzz::scene
// Scene transition and system update order management
#include "Engine/Scene/SceneManager.hpp"
#include "Engine/Scene/SceneSerializer.hpp"
#include "Engine/Scene/Systems/TransformSystem.hpp"
#include "Engine/Scene/Systems/PhysicsSystem.hpp"
#include "Engine/Scene/Systems/ScriptSystem.hpp"
#include "Engine/Scene/Systems/AnimatorSystem.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
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
    if (!m_pendingLoad.empty()) {
        m_active = m_factories[m_pendingLoad]();
        m_pendingLoad.clear();
    }

    if (!m_active) return;

    TransformSystem(*m_active);
    PhysicsSystem(*m_active, world, dt);
    if (auto* resources = renderer::ResourceManager::Active())
        AnimatorSystem(*m_active, *resources, dt);
    ScriptSystem(*m_active, dt);
    m_active->FlushDestroyQueue(dt);
}

Scene* SceneManager::GetActive()
{
    return m_active.get();
}

} // namespace fbzz::scene
