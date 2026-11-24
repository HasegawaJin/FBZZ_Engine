/// @file    SceneComponents.cpp
/// @brief   Scene の型別ストレージ、コンポーネントの複製と複製後のスクリプト処理。
/// @author  Hasegawa Jin
/// @date    2026-11-22
#include "SceneComponentStorage.hpp"

#include <Engine/Scene/Managed/Runtime/ManagedScriptAdapter.hpp>
#include <Engine/Scene/Managed/Runtime/ManagedScriptRuntime.hpp>
#include <Engine/Scene/Serialization/SceneSerializer.hpp>
#include <Engine/Scene/Water/WaterInteraction.hpp>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <type_traits>
#include <utility>
#include <vector>

namespace fbzz::scene {

namespace {
std::atomic<uint64_t> s_componentLifetime{0};

void ResetCopiedWaterRuntime(Scene& scene, EntityID destination)
{
    if (WaterComponent* water = scene.GetComponent<WaterComponent>(destination)) {
        water->ResetSurfaceRuntimeState();
        ResetWaterInteractionRuntime(*water);
    }
    if (WaterZoneComponent* zone = scene.GetComponent<WaterZoneComponent>(destination))
        zone->ResetRuntimeState();
}

/// @note 個体専用メッシュへの借用は複製先へ持ち越さず、各生成システムが新しい所有メッシュを再公開する。
void RemapCopiedRuntimeMeshBindings(const Scene& source, EntityID src, Scene& destination, EntityID dst)
{
    auto* object = source.GetGameObject(src);
    if (!object) return;
    ForEachRegisteredComponent([&]<typename T, typename Registration>() {
        if constexpr (requires(const T& component) { component.runtimeMesh; }) {
            if (const auto* component = object->GetComponent<T>())
                UnlinkRuntimeMesh(destination, dst, component->runtimeMesh);
        }
    });
}

}

uint64_t AllocateComponentLifetime()
{
    const uint64_t lifetime = s_componentLifetime.fetch_add(1, std::memory_order_relaxed) + 1;
    assert(lifetime != 0 && "Component lifetime identity exhausted");
    if (lifetime == 0) std::abort();
    return lifetime;
}

ComponentChunkStorage::ComponentChunkStorage() = default;
ComponentChunkStorage::~ComponentChunkStorage() = default;
ComponentChunkStorage::ComponentChunkStorage(ComponentChunkStorage&& other) noexcept = default;
ComponentChunkStorage& ComponentChunkStorage::operator=(ComponentChunkStorage&& other) noexcept = default;
void ComponentChunkStorage::Add(void* memory, void (*deleter)(void*)) { m_chunks.emplace_back(memory, deleter); }
void ComponentChunkStorage::Clear() { m_chunks.clear(); }
size_t ComponentChunkStorage::Count() const { return m_chunks.size(); }
void* ComponentChunkStorage::Get(size_t index) const { return m_chunks[index].get(); }

Scene::ComponentStorage::ComponentStorage() = default;
Scene::ComponentStorage::~ComponentStorage() = default;

void Scene::ComponentStorage::Clear()
{
    std::apply([](auto&... arrays) { (arrays.Clear(), ...); }, m_arrays);
}

void Scene::ComponentStorage::MoveFrom(ComponentStorage& source) noexcept
{
    m_arrays = std::move(source.m_arrays);
}

namespace detail {
#define FBZZ_COMPONENT_ENTRY(Kind, Type, ...) \
ComponentArray<Type>& ComponentStorageAccess<Type>::Get(Scene& scene) { return std::get<ComponentArray<Type>>(scene.m_components->m_arrays); } \
const ComponentArray<Type>& ComponentStorageAccess<Type>::Get(const Scene& scene) { return std::get<ComponentArray<Type>>(scene.m_components->m_arrays); }
#define FBZZ_COMPONENT_FIRST FBZZ_COMPONENT_ENTRY
#include <Engine/Scene/Components/ComponentRegistry.inl>
#undef FBZZ_COMPONENT_FIRST
#undef FBZZ_COMPONENT_ENTRY
}

void Scene::RemoveAllComponents(EntityID id)
{
    std::apply([&](auto&... arrays) {
        const auto remove = [&](auto& array) {
            if (array.Has(id)) array.Remove(id);
        };
        (remove(arrays), ...);
    }, m_components->m_arrays);
}

void Scene::CopyComponentValues(const Scene& source, EntityID src, EntityID dst)
{
    const auto copy = [&]<typename T>(ComponentArray<T>& destination) {
        if constexpr (std::is_copy_constructible_v<T>) {
            const auto& srcArray = source.GetArray<T>();
            if (srcArray.Has(src) && !destination.Has(dst)) destination.Add(dst, srcArray.Get(src));
        }
    };
    std::apply([&](auto&... arrays) { (copy(arrays), ...); }, m_components->m_arrays);

    ResetCopiedWaterRuntime(*this, dst);
    RemapCopiedRuntimeMeshBindings(source, src, *this, dst);
}

void Scene::DuplicateComponents(EntityID src, EntityID dst)
{
    CopyComponentsFrom(*this, src, dst);
}

void Scene::DuplicateComponentsPassive(EntityID src, EntityID dst)
{
    CopyComponentsFromPassive(*this, src, dst);
}

void Scene::CopyComponentsFrom(const Scene& srcScene, EntityID src, EntityID dst)
{
    CopyComponentValues(srcScene, src, dst);
    CopyScriptComponentFrom(srcScene, src, dst);
}

void Scene::CopyComponentsFromPassive(const Scene& srcScene, EntityID src, EntityID dst)
{
    CopyComponentValues(srcScene, src, dst);
    CopyScriptComponentPassive(srcScene, src, dst);
}

void Scene::PreparePassiveCopiedScripts(std::span<const EntityID> owners)
{
    fbzz::scene::PreparePassiveCopiedScripts(*this, owners);
}

void Scene::FinalizePassiveCopiedScripts(std::span<const EntityID> owners)
{
    fbzz::scene::FinalizePassiveCopiedScripts(*this, owners);
}

void Scene::CopyScriptComponentFrom(const Scene& srcScene, EntityID src, EntityID dst)
{
    const auto& srcArr = srcScene.GetArray<ScriptComponent>();
    auto&       dstArr = GetArray<ScriptComponent>();
    if (!srcArr.Has(src) || dstArr.Has(dst)) return;

    ScriptComponent cloned =
        CloneScriptComponent(srcArr.Get(src), &srcScene, this, GetGameObject(dst));
    if (cloned.scripts.empty()) return;
    dstArr.Add(dst, std::move(cloned));
    struct Identity { std::string entry; std::string birth; };
    std::vector<Identity> identities;
    for (auto& entry : dstArr.Get(dst).scripts) {
        if (!entry.script) continue;
        identities.push_back({entry.inspectionId, entry.script->InspectionId()});
        if (auto* adapter = ManagedScriptRuntime::Instance().FindAdapter(entry.script.get())) adapter->RefreshOwnerBinding();
    }
    /// @note コピー中の forward/self 参照は全 managed 個体の binding が揃ってから接続する。
    for (auto& entry : dstArr.Get(dst).scripts)
        if (auto* adapter = ManagedScriptRuntime::Instance().FindAdapter(entry.script.get())) adapter->ResolveSavedComponentReferences();
    for (const auto& identity : identities) {
        if (!dstArr.Has(dst)) break;
        auto& entries = dstArr.Get(dst).scripts;
        const auto found = std::find_if(entries.begin(), entries.end(), [&](const ScriptEntry& entry) {
            return entry.inspectionId == identity.entry && !entry.m_pendingRemoval && entry.script && entry.script->InspectionId() == identity.birth;
        });
        if (found != entries.end()) found->script->ExecuteProfiledCallback(&Script::OnAfterDeserialize, ScriptCallbackKind::AFTER_DESERIALIZE, "OnAfterDeserialize");
    }
}

void Scene::CopyScriptComponentPassive(const Scene& srcScene, EntityID src, EntityID dst)
{
    const auto& srcArr = srcScene.GetArray<ScriptComponent>();
    auto& dstArr = GetArray<ScriptComponent>();
    if (!srcArr.Has(src) || dstArr.Has(dst)) return;
    ScriptComponent copied = CloneScriptComponentPassive(srcArr.Get(src), &srcScene);
    if (!copied.scripts.empty()) dstArr.Add(dst, std::move(copied));
}

}
