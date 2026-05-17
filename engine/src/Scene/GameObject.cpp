// FBZZ Engine
// GameObject.cpp | fbzz::scene
// GameObject の非 template メソッド実装
#include "engine/Scene/Scene.hpp"
#include "engine/Scene/SceneManager.hpp"
#include "engine/Core/Application.hpp"
#include <algorithm>
#include <cassert>

namespace fbzz::scene {

void GameObject::SetActive(bool active) { m_isActive = active; }
bool GameObject::activeSelf()           const { return m_isActive; }

bool GameObject::CompareTag(const std::string& t) const { return tag == t; }

bool GameObject::IsValid() const {
    return m_scene && m_scene->IsValid(m_id);
}

// -----------------------------------------------------------------------
// 親子関係
// -----------------------------------------------------------------------
void GameObject::SetParent(GameObject& parent) {
    assert(m_scene == parent.m_scene && "異なる Scene 間の親子関係は設定できません");

    // 旧親から切り離す
    if (m_parent.IsValid()) {
        if (auto* oldParent = m_scene->GetGameObject(m_parent)) {
            auto& pc = oldParent->m_children;
            pc.erase(std::remove(pc.begin(), pc.end(), m_id), pc.end());
        }
    }

    m_parent = parent.m_id;
    parent.m_children.push_back(m_id);
}

GameObject* GameObject::GetParent() const {
    if (!m_scene || !m_parent.IsValid()) return nullptr;
    return m_scene->GetGameObject(m_parent);
}

int GameObject::GetChildCount() const {
    return static_cast<int>(m_children.size());
}

GameObject* GameObject::GetChild(int index) const {
    if (!m_scene || index < 0 || index >= static_cast<int>(m_children.size()))
        return nullptr;
    return m_scene->GetGameObject(m_children[index]);
}

// -----------------------------------------------------------------------
// static 検索・Destroy
// Application::Get().GetSceneManager().GetActive() 経由で Scene を取得する
// SceneManager が Application に統合された後に完全実装する
// -----------------------------------------------------------------------
GameObject* GameObject::Find(const std::string& n) {
    auto* scene = core::Application::Get().GetSceneManager().GetActive();
    return scene ? scene->Find(n) : nullptr;
}

GameObject* GameObject::FindWithTag(const std::string& t) {
    auto* scene = core::Application::Get().GetSceneManager().GetActive();
    return scene ? scene->FindWithTag(t) : nullptr;
}

void GameObject::Destroy(GameObject& go, float delay) {
    assert(go.m_scene && "GameObject が Scene に紐付いていません");
    // delay=0 でも次フレームで削除 (毎フレーム末尾の FlushDestroyQueue が処理する)
    go.m_scene->m_destroyQueue.push_back({ go.m_id, delay });
}

} // namespace fbzz::scene
