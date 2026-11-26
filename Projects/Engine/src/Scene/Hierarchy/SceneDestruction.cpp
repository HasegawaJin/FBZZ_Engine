/// @file    SceneDestruction.cpp
/// @brief   Scene の個体破棄、所有範囲の照合、遅延キューとスクリプト通知。
/// @author  Hasegawa Jin
/// @date    2026-11-22
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Rendering/SceneRenderResources.hpp>
#include <Engine/Scene/Scripting/ScriptComponent.hpp>
#include <Engine/Scene/Managed/Runtime/ManagedScriptRuntime.hpp>
#include <algorithm>

namespace fbzz::scene {
namespace {

bool EntityIndexLess(EntityID left, EntityID right)
{
    return left.index < right.index;
}

/// @note 世代付き ID を保持する。コールバックで同じ添字に生成された別個体は所有対象に加えない。
std::vector<EntityID> SnapshotOwnedDestruction(const Scene& scene, std::span<const EntityID> ids)
{
    std::vector<EntityID> owned;
    owned.reserve(ids.size());
    for (const auto id : ids) {
        if (scene.GetGameObject(id)) {
            owned.push_back(id);
        }
    }
    std::sort(owned.begin(), owned.end(), EntityIndexLess);
    owned.erase(std::unique(owned.begin(), owned.end()), owned.end());
    return owned;
}

bool OwnsEntity(std::span<const EntityID> owned, EntityID id)
{
    const auto found = std::lower_bound(owned.begin(), owned.end(), id, EntityIndexLess);
    return found != owned.end() && *found == id;
}

struct DestroyScriptTarget {
    EntityID entity;
    std::string entryId;
    std::string scriptId;
};

/// @note コールバック開始前の個体だけを捕捉し、追加・再配置・同じスロットの置換を別個体として扱う。
void SnapshotDestroyScripts(Scene& scene, EntityID entity, std::vector<DestroyScriptTarget>& targets)
{
    const auto* component = scene.GetComponent<ScriptComponent>(entity);
    if (!component) {
        return;
    }
    for (const auto& entry : component->scripts)
        if (entry.script && (entry.m_awoken || entry.m_started)) {
            targets.push_back({entity, entry.inspectionId, entry.script->InspectionId()});
        }
}

void InvokeDestroyScripts(Scene& scene, const std::vector<DestroyScriptTarget>& targets)
{
    for (const auto& target : targets) {
        auto* component = scene.GetComponent<ScriptComponent>(target.entity);
        if (!component) {
            continue;
        }
        const auto found = std::find_if(component->scripts.begin(), component->scripts.end(), [&](const ScriptEntry& entry) {
            return entry.inspectionId == target.entryId && entry.script && entry.script->InspectionId() == target.scriptId;
        });
        if (found == component->scripts.end() || (!found->m_awoken && !found->m_started)) {
            continue;
        }
        found->script->ExecuteProfiledCallback(&Script::OnDestroy, ScriptCallbackKind::DESTROY, "OnDestroy");
    }
}

}

bool Scene::OwnedChildrenMatch(const GameObject& object, std::span<const EntityID> owned) const
{
    for (const auto child : object.m_children) {
        if (GetGameObject(child) && !OwnsEntity(owned, child)) {
            return false;
        }
    }
    return true;
}

/// @note 全件をコールバック前に検査し、所有外の子がいれば部分的な破棄を開始しない。
bool Scene::ValidateOwnedDestruction(std::span<const EntityID> owned) const
{
    for (const auto id : owned) {
        if (const auto* object = GetGameObject(id); object && !OwnedChildrenMatch(*object, owned)) {
            return false;
        }
    }
    return true;
}

bool Scene::DestroyImmediate(EntityID id, const std::vector<EntityID>* ownership)
{
    auto* go = GetGameObject(id);
    if (!go || go->m_destroying) {
        return true;
    }
    const auto childrenOwned = [&] { return !ownership || OwnedChildrenMatch(*go, *ownership); };
    if ((ownership && !OwnsEntity(*ownership, id)) || !childrenOwned()) {
        return false;
    }
    const auto detachFromParent = [&] {
        auto* parent = GetGameObject(go->m_parent);
        if (!parent) {
            return false;
        }
        std::erase(parent->m_children, id);
        return true;
    };

    go->m_destroying = true;
    const uint64_t generation = m_renderSceneGeneration;
    const auto isOriginalObject = [&] {
        return generation == m_renderSceneGeneration && GetGameObject(id) == go;
    };
    std::vector<DestroyScriptTarget> destroyScripts;
    SnapshotDestroyScripts(*this, id, destroyScripts);

    // 子の通知で階層が変わっても、初めに捕捉した子を順に処理する。
    const auto children = go->m_children;
    for (const auto child : children) {
        if (!DestroyImmediate(child, ownership)) {
            if (isOriginalObject()) {
                go->m_destroying = false;
            }
            return false;
        }
        if (generation != m_renderSceneGeneration) {
            return false;
        }
    }
    if (!isOriginalObject()) {
        return false;
    }
    if (!childrenOwned()) { go->m_destroying = false; return false; }

    // 通常は通知前に親から切り離す。所有付き破棄では通知による子の変更を再検査してから切り離す。
    if (!ownership && detachFromParent()) {
        ++m_hierarchyRevision;
    }
    InvokeDestroyScripts(*this, destroyScripts);
    if (!isOriginalObject()) {
        return false;
    }
    if (!childrenOwned()) { go->m_destroying = false; return false; }
    if (ownership) {
        (void)detachFromParent();
    }

    // Component が抱えている GPU リソースを、Component を捨てる前に返す。
    ReleaseEntityOwnedGpuResources(*this, id);
    ManagedScriptRuntime::RetireObject(*this, id);
    RemoveAllComponents(id);

    m_entityToGameObject[id.index] = nullptr;
    ++m_generations[id.index];
    m_freeIndices.push_back(id.index);

    // 検索やコールバックには即時に退役を公開し、所有配列を詰めるのは最外の操作まで遅らせる。
    const size_t storageIndex = go->m_sceneStorageIndex;
    --m_gameObjectCount;
    m_gameObjects[storageIndex].reset();
    ++m_hierarchyRevision;
    return true;
}

void Scene::CompactGameObjects()
{
    if (m_gameObjectCount == m_gameObjects.size()) {
        return;
    }
    std::erase_if(m_gameObjects, [](const auto& object) { return !object; });
    for (size_t index = 0; index < m_gameObjects.size(); ++index) {
        m_gameObjects[index]->m_sceneStorageIndex = index;
    }
    m_rendererOwnerRevision = AllocateComponentLifetime();
}

/// @note Scene 世代が変わったら、残った要求が同じ ID の新しい個体に届かないよう打ち切る。
bool Scene::DestroyBatch(std::span<const EntityID> pending, const std::vector<EntityID>* ownership)
{
    const uint64_t generation = m_renderSceneGeneration;
    bool complete = true;
    ++m_destroyBatchDepth;
    for (const auto id : pending) {
        if (generation != m_renderSceneGeneration) {
            complete = false;
            break;
        }
        if (!DestroyImmediate(id, ownership) && ownership) {
            complete = false;
            break;
        }
    }
    if (--m_destroyBatchDepth == 0) {
        CompactGameObjects();
    }
    return complete;
}

bool Scene::DestroyGameObject(EntityID id)
{
    if (!IsValid(id)) {
        return false;
    }
    (void)DestroyBatch(std::span<const EntityID>{&id, 1});
    return true;
}

void Scene::DestroyGameObjects(std::span<const EntityID> ids)
{
    // Component の密 ID 列を借りた入力でも、コールバックによる削除で要求を失わないようコピーする。
    const std::vector<EntityID> pending(ids.begin(), ids.end());
    (void)DestroyBatch(pending);
}

bool Scene::CanDestroyOwnedGameObjects(std::span<const EntityID> ids) const
{
    const auto owned = SnapshotOwnedDestruction(*this, ids);
    return ValidateOwnedDestruction(owned);
}

bool Scene::DestroyOwnedGameObjects(std::span<const EntityID> ids)
{
    const auto owned = SnapshotOwnedDestruction(*this, ids);
    if (!ValidateOwnedDestruction(owned)) {
        return false;
    }
    return DestroyBatch(owned, &owned);
}

void Scene::FlushDestroyQueue(float dt)
{
    for (auto& entry : m_destroyQueue) {
        entry.delay -= dt;
    }

    // 通知より先に今回の対象をキューから除く。通知から追加した要求はこの走査に含めない。
    std::vector<EntityID> toDestroy;
    std::erase_if(m_destroyQueue, [&](const DestroyEntry& entry) {
        if (entry.delay <= 0.0f) {
            toDestroy.push_back(entry.id);
            return true;
        }
        return false;
    });
    (void)DestroyBatch(toDestroy);
}

void Scene::InvokeClearDestroyScripts()
{
    // 全対象を先に捕捉し、先行 OnDestroy が別の個体に追加したスクリプトはこの Clear で実行しない。
    std::vector<DestroyScriptTarget> destroyScripts;
    for (const auto id : GetEntities<ScriptComponent>()) {
        SnapshotDestroyScripts(*this, id, destroyScripts);
    }
    InvokeDestroyScripts(*this, destroyScripts);
}

}
