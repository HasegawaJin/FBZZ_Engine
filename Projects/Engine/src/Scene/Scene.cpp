/// @file    Scene.cpp
/// @brief   Scene の Entity 管理と GameObject 所有。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @brief EntityID の生成・破棄、Destroy キュー、Component 複製を扱う。
/// @brief フレーム中の削除は遅延させ、System 走査中の参照破壊を避ける。
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include "Engine/Scene/Scene.hpp"
#include <Engine/Scene/ComponentRegistry.hpp>
#include <Engine/Scene/WorldPartition.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include "Engine/Scene/PrefabPool.hpp"
#include "Engine/Scene/SceneRenderResources.hpp"
#include "Engine/Scene/SceneSerializer.hpp"
#include "Engine/Scene/ScriptComponent.hpp"
#include "Engine/Scene/Components/WaterComponent.hpp"
#include "Engine/Scene/Components/WaterZoneComponent.hpp"
#include "Engine/Scene/WaterInteraction.hpp"
#include "Engine/Scene/Managed/ManagedScriptRuntime.hpp"
#include "Engine/Scene/Managed/ManagedScriptAdapter.hpp"
#include "Engine/Scene/Components/MaterialComponent.hpp"
#include "Engine/Core/Logger.hpp"
#include "Engine/Core/Time.hpp"
#include "Engine/Renderer/Material.hpp"
#include "Engine/Renderer/IShader.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include "Engine/Util/Uuid.hpp"
#include <Core/Profiler/ProfileScope.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <string_view>
#include <utility>

namespace fbzz::scene {

namespace {

void RemapCopiedRuntimeMeshBindings(const Scene& source, EntityID src, Scene& destination, EntityID dst);
std::atomic<uint64_t> s_renderSceneGeneration{0};
std::atomic<uint64_t> s_componentLifetime{0};

enum class RendererWorksetMode { LEGACY, REVISION, VALIDATE };

RendererWorksetMode RendererWorksetModeForProcess()
{
    static const RendererWorksetMode mode = [] {
        const char* value = std::getenv("FBZZ_RENDERER_WORKSET_MODE");
        const std::string_view requested = value ? value : "";
        const auto selected = requested == "revision" ? RendererWorksetMode::REVISION
            : requested == "validate" ? RendererWorksetMode::VALIDATE : RendererWorksetMode::LEGACY;
        const char* name = selected == RendererWorksetMode::REVISION ? "revision"
            : selected == RendererWorksetMode::VALIDATE ? "validate" : "legacy";
        std::fprintf(stderr, "RENDERER_WORKSET_CONFIG mode=%s\n", name);
        return selected;
    }();
    return mode;
}

struct RendererOrderWitness {
    EntityID m_entity;
    size_t m_ownerIndex = SIZE_MAX;
    bool operator==(const RendererOrderWitness&) const = default;
};

/// @note Scene のアドレスは識別にだけ使う。構造識別値が一致しないときだけ現在の密 ID 列・所有位置を再照合する。
struct RendererEntityIndex {
    const Scene* m_scene = nullptr;
    uint64_t m_generation = 0;
    std::array<std::vector<RendererOrderWitness>, 3> m_sources;
    std::array<std::vector<EntityID>, 3> m_ordered;
    std::array<bool, 3> m_ready{};
    std::array<uint64_t, 3> m_membershipRevisions{};
    std::array<uint64_t, 3> m_ownerRevisions{};
    bool m_revisionRejected = false;
    /// @note validate だけで確保し、構造識別値を使わない旧照合の独立状態を保持する。
    std::unique_ptr<RendererEntityIndex> m_legacyOracle;

    void Reset(const Scene& scene)
    {
        m_scene = &scene;
        m_generation = scene.GetRenderSceneGeneration();
        for (auto& source : m_sources) source.clear();
        for (auto& ordered : m_ordered) ordered.clear();
        m_ready.fill(false);
        m_membershipRevisions.fill(0);
        m_ownerRevisions.fill(0);
        m_revisionRejected = false;
        m_legacyOracle.reset();
    }
};

/// @note ID と所有順だけの sidecar はスレッドごと最大 4 Scene に閉じ、退役した Scene や Component を参照しない。
RendererEntityIndex& RendererIndexFor(const Scene& scene)
{
    static thread_local std::array<RendererEntityIndex, 4> indices;
    static thread_local size_t nextIndex = 0;
    for (auto& index : indices) {
        if (index.m_scene != &scene) continue;
        if (index.m_generation != scene.GetRenderSceneGeneration()) index.Reset(scene);
        return index;
    }
    auto& index = indices[nextIndex];
    nextIndex = (nextIndex + 1) % indices.size();
    index.Reset(scene);
    return index;
}

struct DestroyScriptTarget { EntityID entity; std::string entryId; std::string scriptId; };
/// @note callback 開始前の個体だけを捕捉し、追加・再配置・同じ slot の置換を別個体として扱う。
void SnapshotDestroyScripts(Scene& scene, EntityID entity, std::vector<DestroyScriptTarget>& targets)
{
    const auto* component = scene.GetComponent<ScriptComponent>(entity);
    if (!component) return;
    for (const auto& entry : component->scripts)
        if (entry.script && (entry.m_awoken || entry.m_started))
            targets.push_back({entity, entry.inspectionId, entry.script->InspectionId()});
}
void InvokeDestroyScripts(Scene& scene, const std::vector<DestroyScriptTarget>& targets)
{
    for (const auto& target : targets) {
        auto* component = scene.GetComponent<ScriptComponent>(target.entity);
        if (!component) continue;
        const auto found = std::find_if(component->scripts.begin(), component->scripts.end(), [&](const ScriptEntry& entry) {
            return entry.inspectionId == target.entryId && entry.script && entry.script->InspectionId() == target.scriptId;
        });
        if (found == component->scripts.end() || (!found->m_awoken && !found->m_started)) continue;
        found->script->ExecuteProfiledCallback(&Script::OnDestroy, ScriptCallbackKind::DESTROY, "OnDestroy");
    }
}

void ResetCopiedWaterRuntime(Scene& scene, EntityID destination)
{
    if (WaterComponent* water = scene.GetComponent<WaterComponent>(destination)) {
        water->ResetSurfaceRuntimeState();
        ResetWaterInteractionRuntime(*water);
    }
    if (WaterZoneComponent* zone = scene.GetComponent<WaterZoneComponent>(destination))
        zone->ResetRuntimeState();
}

/// @brief Component 個体が抱えている GPU リソースを、返す先がまだ生きているあいだだけ返す。
/// @note 作成元が先に終了した資源は返却せず、別の Manager へ返さない。
void ReleaseGpuResourcesIfPossible(Scene& scene)
{
    ReleaseSceneOwnedGpuResources(scene);
}

} /// @note namespace

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

struct Scene::ComponentStorage {
    template<typename Tuple> struct Arrays;
    template<typename... Ts> struct Arrays<std::tuple<Ts...>> { using Type = std::tuple<ComponentArray<Ts>...>; };
    typename Arrays<ComponentList>::Type m_arrays;
};

namespace detail {
#define FBZZ_COMPONENT_ENTRY(Kind, Type, ...) \
ComponentArray<Type>& ComponentStorageAccess<Type>::Get(Scene& scene) { return std::get<ComponentArray<Type>>(scene.m_components->m_arrays); } \
const ComponentArray<Type>& ComponentStorageAccess<Type>::Get(const Scene& scene) { return std::get<ComponentArray<Type>>(scene.m_components->m_arrays); }
#define FBZZ_COMPONENT_FIRST FBZZ_COMPONENT_ENTRY
#include <Engine/Scene/ComponentRegistry.inl>
#undef FBZZ_COMPONENT_FIRST
#undef FBZZ_COMPONENT_ENTRY
} /// @note namespace detail

Scene::Scene() : m_renderSceneGeneration(++s_renderSceneGeneration),
    m_rendererOwnerRevision(AllocateComponentLifetime()), m_components(std::make_unique<ComponentStorage>()) {}

Scene::~Scene()
{
    ForgetWorldPartitionRuntime(*this);
    PrefabPool::Clear(*this);
    ManagedScriptRuntime::RetireScene(*this);
    /// @note 終了済み subsystem を Script が触らないよう、破棄では Clear / OnDestroy を呼ばない。
    ReleaseGpuResourcesIfPossible(*this);
}

void Scene::SetResourceManager(renderer::ResourceManager* resources)
{
    m_resourceOwner = resources ? resources->GetInstanceId() : 0;
}

renderer::ResourceManager* Scene::GetResourceManager() const
{
    return renderer::ResourceManager::FindOwner(m_resourceOwner);
}

Scene::Scene(Scene&& other) noexcept : Scene()
{
    *this = std::move(other);
}

Scene& Scene::operator=(Scene&& other) noexcept
{
    if (this == &other) return *this;
    assert(m_destroyBatchDepth == 0 && other.m_destroyBatchDepth == 0 && "Scene move requires no active destruction callback");


    Clear();

    m_generations = std::move(other.m_generations);
    m_resourceOwner = std::exchange(other.m_resourceOwner, 0);
    m_nextIndex = other.m_nextIndex;
    m_freeIndices = std::move(other.m_freeIndices);
    m_gameObjects = std::move(other.m_gameObjects);
    m_gameObjectCount = other.m_gameObjectCount;
    m_entityToGameObject = std::move(other.m_entityToGameObject);

    m_components->m_arrays       = std::move(other.m_components->m_arrays);
    m_destroyQueue = std::move(other.m_destroyQueue);
    m_runtimePostProcessSettings = std::move(other.m_runtimePostProcessSettings);
    m_userRenderPasses = std::move(other.m_userRenderPasses);
    m_scriptDebugDrawCommands = std::move(other.m_scriptDebugDrawCommands);
    m_lastScriptDebugDrawTickFrame = other.m_lastScriptDebugDrawTickFrame;
    m_environment = other.m_environment;
    /// @note 同一フレームでも移動元と内容が異なるため、流れは移動先で集め直す。
    InvalidateFlowFrame();

    FixupOwnership();
    TransferWorldPartitionRuntime(other, *this);
    other.Clear();
    return *this;
}

EntityID Scene::TryAllocateEntity() {
    if (m_freeIndices.empty() && m_nextIndex >= MAX_ENTITIES)
        return EntityID::INVALID;

    uint32_t idx;
    if (!m_freeIndices.empty()) {
        idx = m_freeIndices.back();
        m_freeIndices.pop_back();
    } else {
        m_generations.push_back(0);
        m_entityToGameObject.push_back(nullptr);
        idx = m_nextIndex++;
    }
    ++m_generations[idx];
    return EntityID{ idx, m_generations[idx] };
}

bool Scene::DestroyImmediate(EntityID id, const std::vector<EntityID>* ownership) {
    GameObject* go = GetGameObject(id);
    if (!go || go->m_destroying) return true;
    const auto allowed = [&](EntityID value) {
        if (!ownership) return true;
        const auto found = std::lower_bound(ownership->begin(), ownership->end(), value.index,
            [](EntityID candidate, uint32_t index) { return candidate.index < index; });
        return found != ownership->end() && *found == value;
    };
    const auto childrenOwned = [&] {
        if (!ownership) return true;
        for (const auto child : go->m_children) if (IsValid(child) && !allowed(child)) return false;
        return true;
    };
    if (!allowed(id) || !childrenOwned()) return false;
    go->m_destroying = true;
    const uint64_t generation = m_renderSceneGeneration;
    std::vector<DestroyScriptTarget> destroyScripts;
    SnapshotDestroyScripts(*this, id, destroyScripts);

    /// @note 子を再帰的に破棄
    if (go) {
        /// @note コピーして走査
        std::vector<EntityID> children = go->m_children;
        for (EntityID child : children) {
            if (!DestroyImmediate(child, ownership)) { if (GetGameObject(id) == go) go->m_destroying = false; return false; }
            if (generation != m_renderSceneGeneration) return false;
        }

        if (generation != m_renderSceneGeneration || GetGameObject(id) != go) return false;
        if (!childrenOwned()) { go->m_destroying = false; return false; }

        /// @note 親から切り離す
        if (!ownership && go->m_parent.IsValid()) {
            if (auto* parent = GetGameObject(go->m_parent)) {
                auto& pc = parent->m_children;
                pc.erase(std::remove(pc.begin(), pc.end(), id), pc.end());
                ++m_hierarchyRevision;
            }
        }
    }

    InvokeDestroyScripts(*this, destroyScripts);
    /// @note callback 後も個体の正確な世代を照合し、退役した旧個体や再利用 slot を二度解放しない。
    if (generation != m_renderSceneGeneration || GetGameObject(id) != go) return false;
    if (!childrenOwned()) { go->m_destroying = false; return false; }
    if (ownership && go->m_parent.IsValid()) {
        if (auto* parent = GetGameObject(go->m_parent)) {
            auto& children = parent->m_children;
            children.erase(std::remove(children.begin(), children.end(), id), children.end());
        }
    }

    /// @note Component が抱えている GPU リソースを、Component を捨てる前に返す。
    ReleaseEntityOwnedGpuResources(*this, id);

    ManagedScriptRuntime::RetireObject(*this, id);
    /// @note Component 削除
    RemoveAllComponents(id);

    /// @note Entity 解放
    m_entityToGameObject[id.index] = nullptr;
    /// @note generation をインクリメントして古い参照を無効化
    ++m_generations[id.index];
    m_freeIndices.push_back(id.index);

    /// @note 検索・callback には即時に退役を公開し、配列の整理だけを最外の操作までまとめる。
    const size_t storageIndex = go->m_sceneStorageIndex;
    --m_gameObjectCount;
    m_gameObjects[storageIndex].reset();
    ++m_hierarchyRevision;
    return true;
}

void Scene::CompactGameObjects()
{
    if (m_gameObjectCount == m_gameObjects.size()) return;
    m_gameObjects.erase(std::remove_if(m_gameObjects.begin(), m_gameObjects.end(),
        [](const auto& object) { return !object; }), m_gameObjects.end());
    for (size_t index = 0; index < m_gameObjects.size(); ++index)
        m_gameObjects[index]->m_sceneStorageIndex = index;
    m_rendererOwnerRevision = AllocateComponentLifetime();
}

bool Scene::IsValid(EntityID id) const {
    if (!id.IsValid()) return false;
    if (id.index >= m_nextIndex) return false;
    return m_generations[id.index] == id.generation;
}

GameObject* Scene::TryCreateGameObject(const std::string& name) {
    const EntityID id = TryAllocateEntity();
    if (!id.IsValid()) {
        FBZZ_LOG_WARN("Scene::TryCreateGameObject: EntityID range %u exhausted; cannot create '%s'",
                      MAX_ENTITIES, name.c_str());
        return nullptr;
    }

    auto go            = std::make_unique<GameObject>();
    go->name           = name;
    go->instanceId     = util::GenerateUUID();
    go->m_id           = id;
    go->m_scene        = this;
    go->m_sceneStorageIndex = m_gameObjects.size();

    GameObject* ptr = go.get();
    m_entityToGameObject[id.index] = ptr;
    m_gameObjects.push_back(std::move(go));
    ++m_gameObjectCount;
    ++m_hierarchyRevision;

    return ptr;
}

GameObject& Scene::CreateGameObject(const std::string& name) {
    GameObject* gameObject = TryCreateGameObject(name);
    assert(gameObject && "CreateGameObject requires available capacity; use TryCreateGameObject");
    if (!gameObject) std::abort();
    return *gameObject;
}

size_t Scene::RemainingEntityCapacity() const {
    return static_cast<size_t>(MAX_ENTITIES - m_nextIndex) + m_freeIndices.size();
}

bool Scene::CanCreateGameObjects(size_t count) const {
    return count <= RemainingEntityCapacity();
}

GameObject* Scene::FindByGuid(const std::string& guid) const {
    if (guid.empty()) return nullptr;
    for (const auto& go : m_gameObjects)
        if (go && go->instanceId == guid) return go.get();
    return nullptr;
}

/// @brief 検索
GameObject* Scene::Find(const std::string& name) const {
    for (auto& go : m_gameObjects)
        if (go && go->name == name) return go.get();
    return nullptr;
}

GameObject* Scene::FindWithTag(const std::string& t) const {
    for (auto& go : m_gameObjects)
        if (go && go->tag == t) return go.get();
    return nullptr;
}

GameObject* Scene::FindWithLayer(int layer) const {
    for (auto& go : m_gameObjects)
        if (go && go->layer == layer) return go.get();
    return nullptr;
}

std::vector<GameObject*> Scene::FindAllWithTag(const std::string& t) const {
    std::vector<GameObject*> result;
    for (auto& go : m_gameObjects)
        if (go && go->tag == t) result.push_back(go.get());
    return result;
}

std::vector<GameObject*> Scene::FindAllWithLayer(int layer) const {
    std::vector<GameObject*> result;
    for (auto& go : m_gameObjects)
        if (go && go->layer == layer) result.push_back(go.get());
    return result;
}

std::vector<GameObject*> Scene::GetRootGameObjects() const {
    std::vector<GameObject*> result;
    for (auto& go : m_gameObjects)
        if (go && !go->m_parent.IsValid())
            result.push_back(go.get());
    return result;
}

/// @brief イテレーション
GameObjectRange Scene::GameObjects() {
    return GameObjectRange(m_gameObjects.begin(), m_gameObjects.end());
}

void Scene::SortEntitiesBySceneOrder(std::span<EntityID> entities) const
{
    std::sort(entities.begin(), entities.end(), [this](EntityID left, EntityID right) {
        const auto* leftObject = GetGameObject(left);
        const auto* rightObject = GetGameObject(right);
        if (leftObject && rightObject)
            return leftObject->m_sceneStorageIndex < rightObject->m_sceneStorageIndex;
        if (leftObject || rightObject) return leftObject != nullptr;
        if (left.index != right.index) return left.index < right.index;
        return left.generation < right.generation;
    });
}

void Scene::CopyRendererEntities(std::vector<EntityID>& output, RendererEntitySet set) const
{
    FBZZ_PROFILE_SCOPE("Scene::RendererCandidates");
    size_t setIndex = 0;
    switch (set) {
    case RendererEntitySet::GEOMETRY: break;
    case RendererEntitySet::SKINNED: setIndex = 1; break;
    case RendererEntitySet::GEOMETRY_AND_TERRAIN: setIndex = 2; break;
    default: output.clear(); return;
    }

    const auto mode = RendererWorksetModeForProcess();
    auto& index = RendererIndexFor(*this);
    const std::array sources{GetEntities<MeshRenderer>(), GetEntities<SkinnedMeshRenderer>(), GetEntities<TerrainComponent>()};
    const std::array revisions{GetArray<MeshRenderer>().GetStructureRevision(),
        GetArray<SkinnedMeshRenderer>().GetStructureRevision(), GetArray<TerrainComponent>().GetStructureRevision()};
    const std::array used{set != RendererEntitySet::SKINNED, true, set == RendererEntitySet::GEOMETRY_AND_TERRAIN};
    bool useRevision = mode != RendererWorksetMode::LEGACY && !index.m_revisionRejected;
    if (useRevision) {
        bool known = m_rendererOwnerRevision != 0;
        for (size_t source = 0; source < used.size(); ++source)
            if (used[source] && !sources[source].empty() && revisions[source] == 0) known = false;
        if (!known) {
            index.m_revisionRejected = true;
            index.m_ready.fill(false);
            useRevision = false;
            std::fprintf(stderr, "RENDERER_WORKSET_FALLBACK reason=unknown_structure_token\n");
        }
    }
    bool refreshed = false;
    const auto ownerIndex = [this](EntityID id) {
        const auto* object = GetGameObject(id);
        return object ? object->m_sceneStorageIndex : SIZE_MAX;
    };
    const auto copyIndex = [&](RendererEntityIndex& target, bool trustRevision, bool profileRebuild) -> const std::vector<EntityID>& {
        const auto refresh = [&](size_t sourceIndex) {
            if (trustRevision && target.m_membershipRevisions[sourceIndex] == revisions[sourceIndex]
                && target.m_ownerRevisions[sourceIndex] == m_rendererOwnerRevision) return false;
            if (trustRevision) refreshed = true;
            const auto entities = sources[sourceIndex];
            auto& previous = target.m_sources[sourceIndex];
            bool changed = previous.size() != entities.size();
            if (!changed) {
                for (size_t position = 0; position < entities.size(); ++position) {
                    if (previous[position].m_entity != entities[position]
                        || previous[position].m_ownerIndex != ownerIndex(entities[position])) {
                        changed = true;
                        break;
                    }
                }
            }
            if (changed) {
                previous.resize(entities.size());
                for (size_t position = 0; position < entities.size(); ++position)
                    previous[position] = {entities[position], ownerIndex(entities[position])};
            }
            target.m_membershipRevisions[sourceIndex] = revisions[sourceIndex];
            target.m_ownerRevisions[sourceIndex] = m_rendererOwnerRevision;
            return changed;
        };

        /// @note span は callback を呼ばないこの照合中だけ借り、索引には ID と所有位置だけをコピーする。
        if (refresh(1)) target.m_ready.fill(false);
        if (used[0] && refresh(0)) {
            target.m_ready[0] = false;
            target.m_ready[2] = false;
        }
        if (used[2] && refresh(2)) target.m_ready[2] = false;

        auto& ordered = target.m_ordered[setIndex];
        const auto rebuild = [&] {
            ordered.clear();
            const auto append = [&](size_t sourceIndex) {
                for (const auto& witness : target.m_sources[sourceIndex]) ordered.push_back(witness.m_entity);
            };
            if (used[0]) append(0);
            append(1);
            if (used[2]) append(2);
            SortEntitiesBySceneOrder(ordered);
            ordered.erase(std::unique(ordered.begin(), ordered.end()), ordered.end());
            target.m_ready[setIndex] = true;
        };
        if (!target.m_ready[setIndex]) {
            if (profileRebuild) {
                FBZZ_PROFILE_SCOPE("Scene::RendererIndexRebuild");
                rebuild();
            } else rebuild();
        }
        return ordered;
    };

    const auto& ordered = copyIndex(index, useRevision, true);
    if (mode == RendererWorksetMode::VALIDATE && useRevision) {
        if (!index.m_legacyOracle) {
            index.m_legacyOracle = std::make_unique<RendererEntityIndex>();
            index.m_legacyOracle->Reset(*this);
        }
        auto& oracle = *index.m_legacyOracle;
        /// @note oracle は現在値の完全照合後に毎回整列し、候補の ready と構造識別値を信用しない。
        oracle.m_ready[setIndex] = false;
        const auto& expected = copyIndex(oracle, false, false);
        bool matches = ordered == expected;
        for (size_t source = 0; source < used.size(); ++source)
            if (used[source] && index.m_sources[source] != oracle.m_sources[source]) matches = false;
        if (!matches) {
            index.m_revisionRejected = true;
            index.m_ready.fill(false);
            useRevision = false;
            std::fprintf(stderr, "RENDERER_WORKSET_FALLBACK reason=validation_mismatch\n");
            copyIndex(index, false, true);
        }
    }
    FBZZ_PROFILE_MARKER(!useRevision ? "Scene::RendererIndexLegacy"
        : refreshed ? "Scene::RendererIndexRevisionRefresh" : "Scene::RendererIndexRevisionReuse");
    output.assign(ordered.begin(), ordered.end());
}

bool Scene::DestroyGameObject(EntityID id)
{
    if (!IsValid(id)) return false;
    ++m_destroyBatchDepth;
    DestroyImmediate(id);
    if (--m_destroyBatchDepth == 0) CompactGameObjects();
    return true;
}

void Scene::DestroyGameObjects(std::span<const EntityID> ids)
{
    /// @note Component の Entity span を入力にしても、その削除で要求の並びを失わない。
    const std::vector<EntityID> pending(ids.begin(), ids.end());
    const uint64_t generation = m_renderSceneGeneration;
    ++m_destroyBatchDepth;
    for (EntityID id : pending) {
        if (generation != m_renderSceneGeneration) break;
        DestroyImmediate(id);
    }
    if (--m_destroyBatchDepth == 0) CompactGameObjects();
}

bool Scene::CanDestroyOwnedGameObjects(std::span<const EntityID> ids) const
{
    std::vector<EntityID> owned;
    owned.reserve(ids.size());
    for (const auto id : ids) if (IsValid(id)) owned.push_back(id);
    std::sort(owned.begin(), owned.end(), [](EntityID a, EntityID b) { return a.index < b.index; });
    owned.erase(std::unique(owned.begin(), owned.end()), owned.end());
    for (const auto id : owned) {
        const auto* object = GetGameObject(id);
        for (const auto child : object->m_children) {
            if (!IsValid(child)) continue;
            const auto found = std::lower_bound(owned.begin(), owned.end(), child.index,
                [](EntityID value, uint32_t index) { return value.index < index; });
            if (found == owned.end() || *found != child) return false;
        }
    }
    return true;
}

bool Scene::DestroyOwnedGameObjects(std::span<const EntityID> ids)
{
    if (!CanDestroyOwnedGameObjects(ids)) return false;
    std::vector<EntityID> owned;
    owned.reserve(ids.size());
    for (const auto id : ids) if (IsValid(id)) owned.push_back(id);
    std::sort(owned.begin(), owned.end(), [](EntityID a, EntityID b) { return a.index < b.index; });
    owned.erase(std::unique(owned.begin(), owned.end()), owned.end());
    const uint64_t generation = m_renderSceneGeneration;
    bool complete = true;
    ++m_destroyBatchDepth;
    for (const auto id : owned) {
        if (generation != m_renderSceneGeneration || !DestroyImmediate(id, &owned)) { complete = false; break; }
    }
    if (--m_destroyBatchDepth == 0) CompactGameObjects();
    return complete;
}

bool Scene::MoveGameObject(EntityID id, int offset)
{
    CompactGameObjects();
    if (offset == 0 || !IsValid(id) || m_gameObjects.empty()) return false;

    auto it = std::find_if(m_gameObjects.begin(), m_gameObjects.end(),
        [id](const auto& go) { return go->GetID() == id; });
    if (it == m_gameObjects.end()) return false;

    const auto currentIndex = static_cast<int>(std::distance(m_gameObjects.begin(), it));
    int targetIndex = currentIndex + offset;
    targetIndex = std::max(0, std::min(targetIndex, static_cast<int>(m_gameObjects.size()) - 1));
    if (targetIndex == currentIndex) return false;

    return MoveGameObjectToIndex(id, static_cast<size_t>(targetIndex));
}

bool Scene::MoveGameObjectToIndex(EntityID id, size_t newIndex)
{
    CompactGameObjects();
    if (!IsValid(id) || m_gameObjects.empty()) return false;

    auto it = std::find_if(m_gameObjects.begin(), m_gameObjects.end(),
        [id](const auto& go) { return go->GetID() == id; });
    if (it == m_gameObjects.end()) return false;

    const size_t currentIndex = static_cast<size_t>(std::distance(m_gameObjects.begin(), it));
    newIndex = std::min(newIndex, m_gameObjects.size() - 1);
    if (newIndex == currentIndex) return false;

    auto moved = std::move(*it);
    m_gameObjects.erase(it);
    m_gameObjects.insert(m_gameObjects.begin() + static_cast<std::ptrdiff_t>(newIndex), std::move(moved));
    ++m_hierarchyRevision;
    const size_t firstChanged = std::min(currentIndex, newIndex);
    const size_t lastChanged = std::max(currentIndex, newIndex);
    for (size_t index = firstChanged; index <= lastChanged; ++index)
        m_gameObjects[index]->m_sceneStorageIndex = index;
    m_rendererOwnerRevision = AllocateComponentLifetime();
    return true;
}

bool Scene::SetRootSiblingIndex(EntityID id, int newRootIndex)
{
    CompactGameObjects();
    if (!IsValid(id)) return false;
    GameObject* target = GetGameObject(id);
    if (!target || target->m_parent.IsValid()) return false;

    /// @note 対象を除いたルート一覧をフラット index 付きで集める
    std::vector<size_t> otherRootFlatIndices;
    size_t currentFlat = SIZE_MAX;
    for (size_t i = 0; i < m_gameObjects.size(); ++i) {
        const auto& go = m_gameObjects[i];
        if (go->m_parent.IsValid()) continue;
        if (go->GetID() == id) { currentFlat = i; continue; }
        otherRootFlatIndices.push_back(i);
    }
    if (currentFlat == SIZE_MAX) return false;

    newRootIndex = std::clamp(newRootIndex, 0, static_cast<int>(otherRootFlatIndices.size()));

    /// @note 挿入先フラット index (対象を取り除いた後の座標系で求める)
    size_t targetFlat;
    if (newRootIndex >= static_cast<int>(otherRootFlatIndices.size())) {
        /// @note 最後のルートより後ろ = 配列末尾
        targetFlat = m_gameObjects.size() - 1;
    } else {
        targetFlat = otherRootFlatIndices[static_cast<size_t>(newRootIndex)];
        /// @note 削除で 1 つ前へ詰まる分を補正
        if (targetFlat > currentFlat) --targetFlat;
    }
    return MoveGameObjectToIndex(id, targetFlat);
}

bool Scene::SyncSiblingFlatOrder(EntityID id)
{
    CompactGameObjects();
    GameObject* go = GetGameObject(id);
    if (!go) return false;
    /// @note ルートは flat 順そのものが正本
    if (!go->m_parent.IsValid()) return true;
    GameObject* parent = GetGameObject(go->m_parent);
    if (!parent) return false;

    const auto& siblings = parent->m_children;
    const auto pos = std::find(siblings.begin(), siblings.end(), id);
    if (pos == siblings.end()) return false;
    const size_t k = static_cast<size_t>(std::distance(siblings.begin(), pos));
    if (siblings.size() <= 1) return true;

    auto flatIndexOf = [this](EntityID target) -> size_t {
        for (size_t i = 0; i < m_gameObjects.size(); ++i)
            if (m_gameObjects[i]->GetID() == target) return i;
        return SIZE_MAX;
    };
    const size_t curFlat = flatIndexOf(id);
    if (curFlat == SIZE_MAX) return false;

    /// @note 挿入位置は削除後の flat 座標で求め、兄弟順を再現する。
    size_t insertPos;
    if (k > 0) {
        size_t prevFlat = flatIndexOf(siblings[k - 1]);
        if (prevFlat == SIZE_MAX) return false;
        if (prevFlat > curFlat) --prevFlat;
        insertPos = prevFlat + 1;
    } else {
        size_t nextFlat = flatIndexOf(siblings[1]);
        if (nextFlat == SIZE_MAX) return false;
        if (nextFlat > curFlat) --nextFlat;
        insertPos = nextFlat;
    }
    if (insertPos == curFlat) return true;
    return MoveGameObjectToIndex(id, insertPos);
}

/// @brief EntityID → GameObject* O(1) 逆引き
GameObject* Scene::GetGameObject(EntityID id) const {
    if (!IsValid(id)) return nullptr;
    return m_entityToGameObject[id.index];
}

/// @brief Destroy キュー処理
void Scene::FlushDestroyQueue(float dt) {
    for (auto& entry : m_destroyQueue)
        entry.delay -= dt;

    std::vector<EntityID> toDestroy;
    m_destroyQueue.erase(
        std::remove_if(m_destroyQueue.begin(), m_destroyQueue.end(),
            [&](DestroyEntry& e) {
                if (e.delay <= 0.0f) { toDestroy.push_back(e.id); return true; }
                return false;
            }),
        m_destroyQueue.end());

    ++m_destroyBatchDepth;
    const uint64_t generation = m_renderSceneGeneration;
    for (EntityID id : toDestroy) {
        if (generation != m_renderSceneGeneration) break;
        DestroyImmediate(id);
    }
    if (--m_destroyBatchDepth == 0) CompactGameObjects();
}

void Scene::Clear()
{
    /// @note 全対象を先に捕捉し、先行 OnDestroy が別 owner へ追加した個体もこの Clear では実行しない。
    std::vector<EntityID> scriptIds(GetEntities<ScriptComponent>().begin(),
                                    GetEntities<ScriptComponent>().end());
    std::vector<DestroyScriptTarget> destroyScripts;
    for (EntityID id : scriptIds) SnapshotDestroyScripts(*this, id, destroyScripts);
    InvokeDestroyScripts(*this, destroyScripts);

    /// @note Component を捨てる前に返す。捨ててからでは、どのハンドルを持っていたか辿れない。
    ManagedScriptRuntime::RetireScene(*this);
    ReleaseGpuResourcesIfPossible(*this);

    /// @note Play / Stop で Scene* と EntityID が再利用されるため、旧内容のプール参照を破棄する。
    PrefabPool::Clear(*this);

    m_destroyQueue.clear();
    m_gameObjects.clear();
    m_gameObjectCount = 0;

    std::apply([](auto&... arrs) { (..., arrs.Clear()); }, m_components->m_arrays);

    m_generations.clear();
    m_entityToGameObject.clear();
    m_nextIndex = 0;
    m_freeIndices.clear();
    ClearRuntimePostProcessSettings();
    ClearUserRenderPasses();
    m_scriptDebugDrawCommands.clear();
    m_lastScriptDebugDrawTickFrame = 0;
    m_environment = SceneEnvironment{};
    m_flowFrame = FlowFieldFrame{};
    m_renderSceneGeneration = ++s_renderSceneGeneration;
    m_rendererOwnerRevision = AllocateComponentLifetime();
    InvalidateFlowFrame();
    ForgetWorldPartitionRuntime(*this);
}

renderer::PostProcessSettings& Scene::GetRuntimePostProcessSettings()
{
    if (!m_runtimePostProcessSettings)
        m_runtimePostProcessSettings = std::make_unique<renderer::PostProcessSettings>();
    return *m_runtimePostProcessSettings;
}

const renderer::PostProcessSettings* Scene::TryGetRuntimePostProcessSettings() const
{
    return m_runtimePostProcessSettings.get();
}

void Scene::SetRuntimePostProcessSettings(const renderer::PostProcessSettings& settings)
{
    if (!m_runtimePostProcessSettings)
        m_runtimePostProcessSettings = std::make_unique<renderer::PostProcessSettings>(settings);
    else
        *m_runtimePostProcessSettings = settings;
}

void Scene::ClearRuntimePostProcessSettings()
{
    m_runtimePostProcessSettings.reset();
}

void Scene::QueueUserRenderPass(UserRenderPassDesc desc)
{
    m_userRenderPasses.push_back(std::move(desc));
}

void Scene::ClearUserRenderPasses()
{
    m_userRenderPasses.clear();
}

const std::vector<UserRenderPassDesc>& Scene::GetUserRenderPasses() const
{
    return m_userRenderPasses;
}

void Scene::QueueScriptDebugDraw(ScriptDebugDrawCommand command)
{
    /// @note 発行フレームを記録して、duration=0 の描画も同一フレーム内の複数ビューに表示する。
    command.frameCreated = Time::frameCount;
    m_scriptDebugDrawCommands.push_back(command);
}

void Scene::TickScriptDebugDrawCommands(float dt)
{
    const uint64_t currentFrame = Time::frameCount;
    if (m_lastScriptDebugDrawTickFrame == currentFrame)
        return;

    m_lastScriptDebugDrawTickFrame = currentFrame;
    for (auto& command : m_scriptDebugDrawCommands) {
        if (command.frameCreated < currentFrame)
            command.duration -= dt;
    }

    m_scriptDebugDrawCommands.erase(
        std::remove_if(m_scriptDebugDrawCommands.begin(), m_scriptDebugDrawCommands.end(),
            [currentFrame](const ScriptDebugDrawCommand& command) {
                return command.frameCreated < currentFrame && command.duration <= 0.0f;
            }),
        m_scriptDebugDrawCommands.end());
}

const std::vector<ScriptDebugDrawCommand>& Scene::GetScriptDebugDrawCommands() const
{
    return m_scriptDebugDrawCommands;
}

void Scene::RemoveAllComponents(EntityID id)
{
    std::apply([&](auto&... arrs) {
        (..., RemoveIfHas(arrs, id));
    }, m_components->m_arrays);
}

void Scene::DuplicateComponents(EntityID src, EntityID dst)
{
    std::apply([&](auto&... arrs) {
        (..., CopyIfHas(arrs, src, dst));
    }, m_components->m_arrays);

    ResetCopiedWaterRuntime(*this, dst);
    RemapCopiedRuntimeMeshBindings(*this, src, *this, dst);
    CopyScriptComponentFrom(*this, src, dst);
}

namespace {
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

void Scene::CopyComponentsFrom(const Scene& srcScene, EntityID src, EntityID dst)
{
    std::apply([&](const auto&... srcArrs) {
        std::apply([&](auto&... dstArrs) {
            (..., CopyFromOtherIfHas(srcArrs, dstArrs, src, dst));
        }, m_components->m_arrays);
    }, srcScene.m_components->m_arrays);

    ResetCopiedWaterRuntime(*this, dst);
    RemapCopiedRuntimeMeshBindings(srcScene, src, *this, dst);
    CopyScriptComponentFrom(srcScene, src, dst);
}

void Scene::DuplicateComponentsPassive(EntityID src, EntityID dst)
{
    std::apply([&](auto&... arrs) {
        (..., CopyIfHas(arrs, src, dst));
    }, m_components->m_arrays);
    ResetCopiedWaterRuntime(*this, dst);
    RemapCopiedRuntimeMeshBindings(*this, src, *this, dst);
    CopyScriptComponentPassive(*this, src, dst);
}

void Scene::CopyComponentsFromPassive(const Scene& srcScene, EntityID src, EntityID dst)
{
    std::apply([&](const auto&... srcArrs) {
        std::apply([&](auto&... dstArrs) {
            (..., CopyFromOtherIfHas(srcArrs, dstArrs, src, dst));
        }, m_components->m_arrays);
    }, srcScene.m_components->m_arrays);
    ResetCopiedWaterRuntime(*this, dst);
    RemapCopiedRuntimeMeshBindings(srcScene, src, *this, dst);
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

void Scene::FixupOwnership()
{
    m_rendererOwnerRevision = AllocateComponentLifetime();
    /// @note move 前の Scene を指す非所有参照と逆引きを新しい所有者へ付け替える。
    for (auto& go : m_gameObjects) {
        if (!go) continue;
        go->m_scene = this;
        m_entityToGameObject[go->m_id.index] = go.get();
    }

    /// @note Script が保持する Scene* / GameObject* も同様に旧ポインタになっているため更新する。
    bool managedReferences = false;
    for (EntityID id : GetArray<ScriptComponent>().Entities()) {
        auto& sc = GetArray<ScriptComponent>().Get(id);
        for (auto& entry : sc.scripts) {
            if (!entry.script) continue;
            entry.script->SetContext(this, GetGameObject(id));
            const std::string_view type = entry.script->GetTypeName();
            managedReferences |= type.starts_with("cs:") || type.starts_with("cs-internal:");
        }
    }
    /// @note 保存参照の再解決は Scene 移動の一部。通常の owner 再束縛では古い wrapper を復活させない。
    if (managedReferences && !ManagedScriptRuntime::Instance().RebindSceneReferences(*this))
        FBZZ_LOG_ERROR("Scene move could not rebind managed saved references.");
}

} /// @note namespace fbzz::scene
