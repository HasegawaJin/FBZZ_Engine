/// @file    SceneOrder.cpp
/// @brief   Scene の格納順と兄弟の表示順を同期する。
/// @author  Hasegawa Jin
/// @date    2026-11-22
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <algorithm>
#include <cstdint>
#include <utility>

namespace fbzz::scene {

bool Scene::MoveGameObject(EntityID id, int offset)
{
    CompactGameObjects();
    const auto* object = GetGameObject(id);
    if (!object || offset == 0) return false;

    const size_t currentIndex = object->m_sceneStorageIndex;
    const size_t lastIndex = m_gameObjects.size() - 1;
    const size_t targetIndex = offset > 0
        ? currentIndex + std::min(static_cast<size_t>(offset), lastIndex - currentIndex)
        : currentIndex - std::min(static_cast<size_t>(-static_cast<int64_t>(offset)), currentIndex);
    return MoveGameObjectToIndex(id, targetIndex);
}

bool Scene::MoveGameObjectToIndex(EntityID id, size_t newIndex)
{
    CompactGameObjects();
    const auto* object = GetGameObject(id);
    if (!object) return false;

    const size_t currentIndex = object->m_sceneStorageIndex;
    newIndex = std::min(newIndex, m_gameObjects.size() - 1);
    if (newIndex == currentIndex) return false;

    /// @note 移動区間だけを詰め直し、次の操作で使う格納添字を移動と同時に更新する。
    auto moved = std::move(m_gameObjects[currentIndex]);
    if (currentIndex < newIndex) {
        for (size_t index = currentIndex; index < newIndex; ++index) {
            m_gameObjects[index] = std::move(m_gameObjects[index + 1]);
            m_gameObjects[index]->m_sceneStorageIndex = index;
        }
    } else {
        for (size_t index = currentIndex; index > newIndex; --index) {
            m_gameObjects[index] = std::move(m_gameObjects[index - 1]);
            m_gameObjects[index]->m_sceneStorageIndex = index;
        }
    }
    m_gameObjects[newIndex] = std::move(moved);
    m_gameObjects[newIndex]->m_sceneStorageIndex = newIndex;
    ++m_hierarchyRevision;
    m_rendererOwnerRevision = AllocateComponentLifetime();
    return true;
}

bool Scene::SetRootSiblingIndex(EntityID id, int newRootIndex)
{
    CompactGameObjects();
    const auto* root = GetGameObject(id);
    if (!root || root->m_parent.IsValid()) return false;

    const size_t currentFlat = root->m_sceneStorageIndex;
    size_t targetRoot = static_cast<size_t>(std::max(newRootIndex, 0));
    for (size_t index = 0; index < m_gameObjects.size(); ++index) {
        if (index == currentFlat || m_gameObjects[index]->m_parent.IsValid()) continue;
        /// @note 対象の除去後に次のルートの直前へ挿入する。
        if (targetRoot == 0)
            return MoveGameObjectToIndex(id, index > currentFlat ? index - 1 : index);
        --targetRoot;
    }
    /// @note 最後のルートは、子の格納位置も含めた配列全体の末尾へ置く。
    return MoveGameObjectToIndex(id, m_gameObjects.size() - 1);
}

bool Scene::SyncSiblingFlatOrder(EntityID id)
{
    CompactGameObjects();
    const auto* object = GetGameObject(id);
    if (!object) return false;
    /// @note ルート順の正本は Scene の格納順。
    if (!object->m_parent.IsValid()) return true;
    const auto* parent = GetGameObject(object->m_parent);
    if (!parent) return false;

    const auto& siblings = parent->m_children;
    const auto position = std::find(siblings.begin(), siblings.end(), id);
    if (position == siblings.end()) return false;
    if (siblings.size() == 1) return true;

    const bool hasPrevious = position != siblings.begin();
    const auto* neighbor = GetGameObject(hasPrevious ? *(position - 1) : *(position + 1));
    if (!neighbor) return false;

    const size_t currentFlat = object->m_sceneStorageIndex;
    const size_t neighborFlat = neighbor->m_sceneStorageIndex;
    /// @note 対象の除去後、直前の兄弟の後ろか、先頭なら次の兄弟の前へ挿入する。
    const size_t neighborAfterRemoval = neighborFlat > currentFlat ? neighborFlat - 1 : neighborFlat;
    const size_t insertPos = hasPrevious ? neighborAfterRemoval + 1 : neighborAfterRemoval;
    return insertPos == currentFlat || MoveGameObjectToIndex(id, insertPos);
}

}
