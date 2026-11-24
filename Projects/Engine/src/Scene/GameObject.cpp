/// @file    GameObject.cpp
/// @brief   GameObject の階層、状態、検索と破棄の操作。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/SceneManager.hpp"
#include "Engine/Scene/Scripting/ScriptRuntime.hpp"
#include <algorithm>
#include <cassert>
#include <cstddef>

namespace fbzz::scene {

void GameObject::SetRuntimeGenerated(bool generated)
{
    if (m_runtimeGenerated == generated) return;
    m_runtimeGenerated = generated;
    if (m_scene) ++m_scene->m_hierarchyRevision;
}

void GameObject::SetActive(bool active) { m_isActive = active; }
bool GameObject::activeSelf() const { return m_isActive; }
bool GameObject::activeInHierarchy() const
{
    if (!m_isActive) return false;
    const GameObject* cur = GetParent();
    while (cur) {
        if (!cur->m_isActive) return false;
        cur = cur->GetParent();
    }
    return true;
}

bool GameObject::CompareTag(const std::string& t) const { return tag == t; }

bool GameObject::IsValid() const
{
    return m_scene && m_scene->IsValid(m_id);
}

void GameObject::SetParent(GameObject& parent)
{
    [[maybe_unused]] const bool changed = SetParent(&parent);
    assert(changed && "Invalid parent relationship");
}

bool GameObject::SetParent(GameObject* parent)
{
    if (!m_scene) return false;
    if (parent) {
        if (m_scene != parent->m_scene || parent == this) return false;
        if (m_parent == parent->m_id) return true;
        if (parent->IsDescendantOf(*this)) return false;
    } else if (!m_parent.IsValid()) {
        return true;
    }

    if (m_parent.IsValid()) {
        if (auto* oldParent = m_scene->GetGameObject(m_parent)) {
            auto& children = oldParent->m_children;
            children.erase(std::remove(children.begin(), children.end(), m_id), children.end());
        }
    }

    m_parent = parent ? parent->m_id : EntityID::INVALID;
    if (parent) parent->m_children.push_back(m_id);
    ++m_scene->m_hierarchyRevision;
    return true;
}

bool GameObject::ClearParent()
{
    return SetParent(nullptr);
}

bool GameObject::IsDescendantOf(const GameObject& ancestor) const
{
    if (m_scene != ancestor.m_scene) return false;

    EntityID current = m_parent;
    while (current.IsValid()) {
        if (current == ancestor.m_id) return true;
        auto* parent = m_scene->GetGameObject(current);
        if (!parent) return false;
        current = parent->m_parent;
    }
    return false;
}

GameObject* GameObject::GetParent() const
{
    if (!m_scene || !m_parent.IsValid()) return nullptr;
    return m_scene->GetGameObject(m_parent);
}

int GameObject::GetChildCount() const
{
    return static_cast<int>(m_children.size());
}

int GameObject::GetSiblingIndex() const
{
    if (!m_scene) return 0;
    if (m_parent.IsValid()) {
        if (auto* parent = m_scene->GetGameObject(m_parent)) {
            const auto& siblings = parent->m_children;
            const auto it = std::find(siblings.begin(), siblings.end(), m_id);
            if (it != siblings.end())
                return static_cast<int>(std::distance(siblings.begin(), it));
        }
        return 0;
    }
    /// @note 非アクティブなルートも順位に含め、退役済みの格納位置は GameObjects が飛ばす。
    int index = 0;
    for (const auto& object : m_scene->GameObjects()) {
        if (&object == this) return index;
        if (!object.m_parent.IsValid()) ++index;
    }
    return 0;
}

bool GameObject::SetSiblingIndex(int index)
{
    if (!m_scene) return false;
    if (m_parent.IsValid()) {
        auto* parent = m_scene->GetGameObject(m_parent);
        if (!parent) return false;
        auto& siblings = parent->m_children;
        const auto it = std::find(siblings.begin(), siblings.end(), m_id);
        if (it == siblings.end()) return false;
        const auto clamped = std::min(static_cast<size_t>(std::max(index, 0)), siblings.size() - 1);
        const auto target = siblings.begin() + static_cast<std::ptrdiff_t>(clamped);
        if (target == it) return false;
        if (it < target) std::rotate(it, it + 1, target + 1);
        else std::rotate(target, it, it + 1);
        ++m_scene->m_hierarchyRevision;
        /// @note 保存・Undo・Play の復元は Scene の格納順で親子を再構築するため、兄弟順と同期する。
        m_scene->SyncSiblingFlatOrder(m_id);
        return true;
    }
    return m_scene->SetRootSiblingIndex(m_id, index);
}

GameObject* GameObject::GetChild(int index) const
{
    if (!m_scene || index < 0 || index >= static_cast<int>(m_children.size()))
        return nullptr;
    return m_scene->GetGameObject(m_children[index]);
}

GameObject* GameObject::FindInSubtree(std::string_view objectName)
{
    if (!IsValid()) return nullptr;
    std::vector<GameObject*> pending{this};
    while (!pending.empty()) {
        GameObject* current = pending.back();
        pending.pop_back();
        if (current->name == objectName) return current;
        for (int i = current->GetChildCount(); i > 0; --i) {
            if (GameObject* child = current->GetChild(i - 1)) pending.push_back(child);
        }
    }
    return nullptr;
}

namespace {
    inline Scene* GetActiveScene()
    {
        auto* mgr = ScriptRuntime::GetCurrent().sceneManager;
        return mgr ? mgr->GetActive() : nullptr;
    }
}

GameObject* GameObject::Find(const std::string& n, bool includeInactive)
{
    auto* s = GetActiveScene();
    if (!s) return nullptr;
    for (auto& go : s->GameObjects())
        if (go.name == n && (includeInactive || go.activeInHierarchy())) return &go;
    return nullptr;
}

GameObject* GameObject::FindByGuid(const std::string& guid)
{
    auto* s = GetActiveScene();
    return s ? s->FindByGuid(guid) : nullptr;
}

GameObject* GameObject::FindWithTag(const std::string& t, bool includeInactive)
{
    auto* s = GetActiveScene();
    if (!s) return nullptr;
    for (auto& go : s->GameObjects())
        if (go.tag == t && (includeInactive || go.activeInHierarchy())) return &go;
    return nullptr;
}

void GameObject::Destroy(GameObject& go, float delay)
{
    assert(go.m_scene && "GameObject is not attached to a Scene");
    go.m_scene->m_destroyQueue.push_back({ go.m_id, delay });
}

}
