// FBZZ Engine
// Scene.cpp | fbzz::scene
// Scene の実装: Entity 管理・GameObject 所有・Destroy キュー処理
#include "Engine/Scene/Scene.hpp"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <utility>

namespace fbzz::scene {

Scene::Scene(Scene&& other) noexcept
{
    *this = std::move(other);
}

Scene& Scene::operator=(Scene&& other) noexcept
{
    if (this == &other) return *this;

    Clear();

    std::memcpy(m_generations, other.m_generations, sizeof(m_generations));
    m_nextIndex = other.m_nextIndex;
    m_freeIndices = std::move(other.m_freeIndices);
    m_gameObjects = std::move(other.m_gameObjects);
    std::memset(m_entityToGameObject, 0, sizeof(m_entityToGameObject));

    m_arrays       = std::move(other.m_arrays);
    m_destroyQueue = std::move(other.m_destroyQueue);

    FixupOwnership();
    other.Clear();
    return *this;
}

// -----------------------------------------------------------------------
// Entity 管理
// -----------------------------------------------------------------------
EntityID Scene::AllocateEntity() {
    uint32_t idx;
    if (!m_freeIndices.empty()) {
        idx = m_freeIndices.back();
        m_freeIndices.pop_back();
    } else {
        assert(m_nextIndex < MAX_ENTITIES && "Entity 数が MAX_ENTITIES を超えました");
        idx = m_nextIndex++;
    }
    ++m_generations[idx];
    return EntityID{ idx, m_generations[idx] };
}

void Scene::DestroyImmediate(EntityID id) {
    if (!IsValid(id)) return;

    GameObject* go = GetGameObject(id);

    // 子を再帰的に破棄
    if (go) {
        std::vector<EntityID> children = go->m_children; // コピーして走査
        for (EntityID child : children)
            DestroyImmediate(child);

        // 親から切り離す
        if (go->m_parent.IsValid()) {
            if (auto* parent = GetGameObject(go->m_parent)) {
                auto& pc = parent->m_children;
                pc.erase(std::remove(pc.begin(), pc.end(), id), pc.end());
            }
        }
    }

    // Component 削除
    RemoveAllComponents(id);

    // Entity 解放
    m_entityToGameObject[id.index] = nullptr;
    ++m_generations[id.index]; // generation をインクリメントして古い参照を無効化
    m_freeIndices.push_back(id.index);

    // m_gameObjects から削除
    if (go) {
        auto it = std::find_if(m_gameObjects.begin(), m_gameObjects.end(),
            [go](const auto& u) { return u.get() == go; });
        if (it != m_gameObjects.end())
            m_gameObjects.erase(it);
    }
}

bool Scene::IsValid(EntityID id) const {
    if (!id.IsValid()) return false;
    if (id.index >= m_nextIndex) return false;
    return m_generations[id.index] == id.generation;
}

// -----------------------------------------------------------------------
// GameObject 生成
// -----------------------------------------------------------------------
GameObject& Scene::CreateGameObject(const std::string& name) {
    EntityID id = AllocateEntity();

    auto go        = std::make_unique<GameObject>();
    go->name       = name;
    go->m_id       = id;
    go->m_scene    = this;

    GameObject* ptr = go.get();
    m_entityToGameObject[id.index] = ptr;
    m_gameObjects.push_back(std::move(go));

    return *ptr;
}

// -----------------------------------------------------------------------
// 検索
// -----------------------------------------------------------------------
GameObject* Scene::Find(const std::string& name) const {
    for (auto& go : m_gameObjects)
        if (go->name == name) return go.get();
    return nullptr;
}

GameObject* Scene::FindWithTag(const std::string& t) const {
    for (auto& go : m_gameObjects)
        if (go->tag == t) return go.get();
    return nullptr;
}

GameObject* Scene::FindWithLayer(int layer) const {
    for (auto& go : m_gameObjects)
        if (go->layer == layer) return go.get();
    return nullptr;
}

std::vector<GameObject*> Scene::FindAllWithTag(const std::string& t) const {
    std::vector<GameObject*> result;
    for (auto& go : m_gameObjects)
        if (go->tag == t) result.push_back(go.get());
    return result;
}

std::vector<GameObject*> Scene::FindAllWithLayer(int layer) const {
    std::vector<GameObject*> result;
    for (auto& go : m_gameObjects)
        if (go->layer == layer) result.push_back(go.get());
    return result;
}

std::vector<GameObject*> Scene::GetRootGameObjects() const {
    std::vector<GameObject*> result;
    for (auto& go : m_gameObjects)
        if (!go->m_parent.IsValid())
            result.push_back(go.get());
    return result;
}

// -----------------------------------------------------------------------
// イテレーション
// -----------------------------------------------------------------------
GameObjectRange Scene::GameObjects() {
    return GameObjectRange(m_gameObjects.begin(), m_gameObjects.end());
}

bool Scene::DestroyGameObject(EntityID id)
{
    if (!IsValid(id)) return false;
    DestroyImmediate(id);
    return true;
}

bool Scene::MoveGameObject(EntityID id, int offset)
{
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
    return true;
}

// -----------------------------------------------------------------------
// EntityID → GameObject* O(1) 逆引き
// -----------------------------------------------------------------------
GameObject* Scene::GetGameObject(EntityID id) const {
    if (!IsValid(id)) return nullptr;
    return m_entityToGameObject[id.index];
}

// -----------------------------------------------------------------------
// Destroy キュー処理
// -----------------------------------------------------------------------
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

    for (EntityID id : toDestroy)
        DestroyImmediate(id);
}

void Scene::Clear()
{
    m_destroyQueue.clear();
    m_gameObjects.clear();

    std::apply([](auto&... arrs) { (..., arrs.Clear()); }, m_arrays);

    std::memset(m_generations,       0, sizeof(m_generations));
    std::memset(m_entityToGameObject, 0, sizeof(m_entityToGameObject));
    m_nextIndex = 0;
    m_freeIndices.clear();
}

void Scene::RemoveAllComponents(EntityID id)
{
    std::apply([&](auto&... arrs) {
        (..., RemoveIfHas(arrs, id));
    }, m_arrays);
}

void Scene::DuplicateComponents(EntityID src, EntityID dst)
{
    std::apply([&](auto&... arrs) {
        (..., CopyIfHas(arrs, src, dst));
    }, m_arrays);
}

void Scene::FixupOwnership()
{
    for (auto& go : m_gameObjects) {
        go->m_scene = this;
        m_entityToGameObject[go->m_id.index] = go.get();
    }

    for (EntityID id : GetArray<ScriptComponent>().Entities()) {
        auto& sc = GetArray<ScriptComponent>().Get(id);
        if (sc.script) sc.script->SetContext(this, GetGameObject(id));
    }
}

} // namespace fbzz::scene
