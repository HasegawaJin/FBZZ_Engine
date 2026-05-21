// FBZZ Engine
// Scene.cpp | fbzz::scene
// Scene の実装: Entity 管理・GameObject 所有・Destroy キュー処理
#include "engine/Scene/Scene.hpp"
#include <algorithm>
#include <cassert>

namespace fbzz::scene {

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
    if (m_meshRenderers.Has(id))     m_meshRenderers.Remove(id);
    if (m_particleEmitters.Has(id))  m_particleEmitters.Remove(id);
    if (m_rigidBodies.Has(id))       m_rigidBodies.Remove(id);
    if (m_skyRenderers.Has(id))      m_skyRenderers.Remove(id);
    if (m_lightComponents.Has(id))   m_lightComponents.Remove(id);
    if (m_cameraComponents.Has(id))  m_cameraComponents.Remove(id);
    if (m_audioSources.Has(id))      m_audioSources.Remove(id);
    if (m_scriptComponents.Has(id))  m_scriptComponents.Remove(id);

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

} // namespace fbzz::scene
