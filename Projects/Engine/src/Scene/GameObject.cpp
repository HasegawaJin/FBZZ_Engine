/// @file    GameObject.cpp
/// @brief   GameObject の非 template メソッド実装。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// active、tag、親子関係、検索、Destroy の OOP API を提供する。
/// Component 操作の template 本体は Scene.hpp 側に置く。
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/SceneManager.hpp"
#include "Engine/Scene/ScriptRuntime.hpp"
#include <algorithm>
#include <cassert>

namespace fbzz::scene {

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
    if (!parent) return ClearParent();
    if (!m_scene || m_scene != parent->m_scene) return false;
    if (parent == this || parent->IsDescendantOf(*this)) return false;
    if (m_parent == parent->m_id) return true;

    if (m_parent.IsValid()) {
        if (auto* oldParent = m_scene->GetGameObject(m_parent)) {
            auto& children = oldParent->m_children;
            children.erase(std::remove(children.begin(), children.end(), m_id), children.end());
        }
    }

    auto& newSiblings = parent->m_children;
    newSiblings.erase(std::remove(newSiblings.begin(), newSiblings.end(), m_id), newSiblings.end());

    m_parent = parent->m_id;
    newSiblings.push_back(m_id);
    return true;
}

bool GameObject::ClearParent()
{
    if (!m_scene) return false;
    if (!m_parent.IsValid()) return true;

    if (auto* oldParent = m_scene->GetGameObject(m_parent)) {
        auto& children = oldParent->m_children;
        children.erase(std::remove(children.begin(), children.end(), m_id), children.end());
    }
    m_parent = EntityID::INVALID;
    return true;
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
    // ルート: Scene のルート一覧における出現順
    int index = 0;
    for (auto* root : m_scene->GetRootGameObjects()) {
        if (root == this) return index;
        ++index;
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
        const int current = static_cast<int>(std::distance(siblings.begin(), it));
        const int clamped = std::clamp(index, 0, static_cast<int>(siblings.size()) - 1);
        if (clamped == current) return false;
        siblings.erase(it);
        siblings.insert(siblings.begin() + clamped, m_id);
        // WHY: シリアライザは flat 順で親子を再構築するため、m_children の並び替えだけでは
        //      保存 / Undo スナップショット / Play 復元で順序が元に戻ってしまう。
        m_scene->SyncSiblingFlatOrder(m_id);
        return true;
    }
    // ルート: フラット配列上のルート順序を Scene 側で並べ替える
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

GameObject* GameObject::Find(const std::string& n)
{
    auto* s = GetActiveScene();
    return s ? s->Find(n) : nullptr;
}

GameObject* GameObject::FindByGuid(const std::string& guid)
{
    auto* s = GetActiveScene();
    return s ? s->FindByGuid(guid) : nullptr;
}

GameObject* GameObject::FindWithTag(const std::string& t)
{
    auto* s = GetActiveScene();
    return s ? s->FindWithTag(t) : nullptr;
}

void GameObject::Destroy(GameObject& go, float delay)
{
    assert(go.m_scene && "GameObject is not attached to a Scene");
    go.m_scene->m_destroyQueue.push_back({ go.m_id, delay });
}

} // namespace fbzz::scene
