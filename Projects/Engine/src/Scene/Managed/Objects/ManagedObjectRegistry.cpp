/// @file    ManagedObjectRegistry.cpp
/// @brief   object token の双方向照会と寿命終了時の有界な回収。
/// @author  Hasegawa Jin
/// @date    2026-10-05
#include "ManagedObjectRegistry.hpp"
#include <Engine/Scene/Scene.hpp>
#include <cassert>
#include <cstdlib>
#include <unordered_map>

namespace fbzz::scene::managed_detail {
struct ManagedObjectRegistry::Impl {
    struct SceneRecords { uint64_t lifetime = 0; std::unordered_map<uint32_t, uint64_t> slots; };
    uint64_t nextToken = 1;
    std::unordered_map<uint64_t, Record> records;
    std::unordered_map<Scene*, SceneRecords> scenes;
};
ManagedObjectRegistry::ManagedObjectRegistry() : m_impl(std::make_unique<Impl>()) {}
ManagedObjectRegistry::~ManagedObjectRegistry() = default;

uint64_t ManagedObjectRegistry::Register(Scene& scene, GameObject& object)
{
    if (object.GetScene() != &scene || !object.IsValid()) return 0;
    const uint64_t lifetime = scene.GetRenderSceneGeneration();
    auto existing = m_impl->scenes.find(&scene);
    if (existing != m_impl->scenes.end() && existing->second.lifetime != lifetime) RetireScene(scene);
    auto& bucket = m_impl->scenes[&scene];
    bucket.lifetime = lifetime;
    const EntityID entity = object.GetID();
    auto slot = bucket.slots.find(entity.index);
    if (slot != bucket.slots.end()) {
        const auto* record = Find(slot->second);
        if (record && record->entity == entity && record->guid == object.instanceId) return slot->second;
        m_impl->records.erase(slot->second);
    }
    const uint64_t token = m_impl->nextToken++;
    assert(token != 0 && "Managed object identity exhausted");
    if (token == 0) std::abort();
    m_impl->records.emplace(token, Record{&scene, lifetime, entity, object.instanceId});
    bucket.slots[entity.index] = token;
    return token;
}
const ManagedObjectRegistry::Record* ManagedObjectRegistry::Find(uint64_t token) const
{
    const auto found = m_impl->records.find(token);
    return found == m_impl->records.end() ? nullptr : &found->second;
}
GameObject* ManagedObjectRegistry::Resolve(Scene& scene, uint64_t token) const
{
    const auto* record = Find(token);
    if (!record || record->scene != &scene || record->sceneGeneration != scene.GetRenderSceneGeneration()) return nullptr;
    GameObject* object = scene.GetGameObject(record->entity);
    return object && object->instanceId == record->guid ? object : nullptr;
}
void ManagedObjectRegistry::RetireObject(Scene& scene, EntityID entity)
{
    const auto bucket = m_impl->scenes.find(&scene);
    if (bucket == m_impl->scenes.end()) return;
    const auto slot = bucket->second.slots.find(entity.index);
    if (slot == bucket->second.slots.end()) return;
    const auto* record = Find(slot->second);
    if (record && record->entity != entity) return;
    m_impl->records.erase(slot->second);
    bucket->second.slots.erase(slot);
    if (bucket->second.slots.empty()) m_impl->scenes.erase(bucket);
}
void ManagedObjectRegistry::RetireScene(Scene& scene)
{
    const auto bucket = m_impl->scenes.find(&scene);
    if (bucket == m_impl->scenes.end()) return;
    for (const auto& [index, token] : bucket->second.slots) m_impl->records.erase(token);
    m_impl->scenes.erase(bucket);
}
void ManagedObjectRegistry::Clear() { m_impl->records.clear(); m_impl->scenes.clear(); }
size_t ManagedObjectRegistry::Count() const { return m_impl->records.size(); }
} /// @note namespace fbzz::scene::managed_detail
