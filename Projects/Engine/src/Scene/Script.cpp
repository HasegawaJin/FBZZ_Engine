// FBZZ Engine
// Script.cpp | fbzz::scene
// Script 基底クラスの便利 API 実装
// template 以外のショートハンドをここに集約し、ヘッダの include 依存を最小化する。
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <cassert>

namespace fbzz::scene {

// ── コンテキスト設定 ────────────────────────────────────────────────────────

void Script::SetContext(Scene* scene, GameObject* gameObject)
{
    m_scene      = scene;
    m_gameObject = gameObject;
    // WHY: transform ポインタを SetContext で同期することで、
    //      派生クラスが m_gameObject->transform と書かずに transform-> で直接アクセスできる。
    transform = gameObject ? &gameObject->transform : nullptr;
}

// ── シーン操作ショートハンド ────────────────────────────────────────────────

GameObject* Script::Find(const std::string& name) const
{
    return m_scene ? m_scene->Find(name) : nullptr;
}

GameObject* Script::FindWithTag(const std::string& tag) const
{
    return m_scene ? m_scene->FindWithTag(tag) : nullptr;
}

GameObject* Script::GetGameObject(EntityID id) const
{
    return m_scene ? m_scene->GetGameObject(id) : nullptr;
}

GameObject& Script::CreateGameObject(const std::string& name) const
{
    assert(m_scene && "Script context is not set");
    return m_scene->CreateGameObject(name);
}

// シーン内の全 GO を走査し、有効なメインカメラを返す。
// WHY: CameraComponent.isMain は複数存在し得るが、通常 1 つ。
//      最初に見つかった有効なものを返す。
GameObject* Script::GetMainCameraObject() const
{
    if (!m_scene) return nullptr;
    for (auto& go : m_scene->GameObjects()) {
        auto* cam = go.GetComponent<CameraComponent>();
        if (cam && cam->enabled && cam->isMain) return &go;
    }
    return nullptr;
}

// ── Unity: Object.Destroy ───────────────────────────────────────────────────

void Script::Destroy(GameObject& go, float delay)
{
    GameObject::Destroy(go, delay);
}

// ── Animator ショートハンド ─────────────────────────────────────────────────

void Script::SetAnimatorFloat(std::string_view name, float v) const
{
    if (!m_gameObject) return;
    auto* anim = m_gameObject->GetComponent<AnimatorComponent>();
    if (anim) anim->SetFloat(name, v);
}

void Script::SetAnimatorInt(std::string_view name, int v) const
{
    if (!m_gameObject) return;
    auto* anim = m_gameObject->GetComponent<AnimatorComponent>();
    if (anim) anim->SetInt(name, v);
}

void Script::SetAnimatorBool(std::string_view name, bool v) const
{
    if (!m_gameObject) return;
    auto* anim = m_gameObject->GetComponent<AnimatorComponent>();
    if (anim) anim->SetBool(name, v);
}

void Script::SetAnimatorTrigger(std::string_view name) const
{
    if (!m_gameObject) return;
    auto* anim = m_gameObject->GetComponent<AnimatorComponent>();
    if (anim) anim->SetTrigger(name);
}

bool Script::IsAnimatorInState(std::string_view name) const
{
    if (!m_gameObject) return false;
    auto* anim = m_gameObject->GetComponent<AnimatorComponent>();
    return anim && anim->IsInState(name);
}

// ── PostProcess ─────────────────────────────────────────────────────────────

renderer::PostProcessSettings& Script::GetRuntimePostProcessSettings()
{
    assert(m_scene && "Script context is not set");
    return m_scene->GetRuntimePostProcessSettings();
}

const renderer::PostProcessSettings* Script::TryGetRuntimePostProcessSettings() const
{
    if (!m_scene) return nullptr;
    return m_scene->TryGetRuntimePostProcessSettings();
}

void Script::SetRuntimePostProcessSettings(const renderer::PostProcessSettings& settings)
{
    assert(m_scene && "Script context is not set");
    m_scene->SetRuntimePostProcessSettings(settings);
}

void Script::ClearRuntimePostProcessSettings()
{
    if (m_scene)
        m_scene->ClearRuntimePostProcessSettings();
}

} // namespace fbzz::scene
