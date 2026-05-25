// FBZZ Engine
// GameObject.cpp | fbzz::scene
// GameObject の非 template メソッド実装
// active、tag、親子関係、検索、Destroy の OOP API を提供する。
// Component 操作の template 本体は Scene.hpp 側に置く。
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/SceneManager.hpp"
#include "Engine/Core/Application.hpp"
#include <algorithm>
#include <cassert>

namespace fbzz::scene {

void GameObject::SetActive(bool active) { m_isActive = active; }
bool GameObject::activeSelf() const { return m_isActive; }

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

GameObject* GameObject::GetChild(int index) const
{
    if (!m_scene || index < 0 || index >= static_cast<int>(m_children.size()))
        return nullptr;
    return m_scene->GetGameObject(m_children[index]);
}

GameObject* GameObject::Find(const std::string& n)
{
    auto* scene = core::Application::Get().GetSceneManager().GetActive();
    return scene ? scene->Find(n) : nullptr;
}

GameObject* GameObject::FindWithTag(const std::string& t)
{
    auto* scene = core::Application::Get().GetSceneManager().GetActive();
    return scene ? scene->FindWithTag(t) : nullptr;
}

void GameObject::Destroy(GameObject& go, float delay)
{
    assert(go.m_scene && "GameObject is not attached to a Scene");
    go.m_scene->m_destroyQueue.push_back({ go.m_id, delay });
}

} // namespace fbzz::scene
