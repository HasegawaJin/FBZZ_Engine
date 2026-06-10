// FBZZ Engine
// Script.cpp | fbzz::scene
// Script 基底クラスの便利 API 実装
// template 以外のショートハンドをここに集約し、ヘッダの include 依存を最小化する。
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Renderer/IShader.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <algorithm>
#include <cassert>
#include <utility>

namespace fbzz::scene {

physics::World*    Script::s_physicsWorld  = nullptr;
Script::PrefabInstantiateFn Script::s_instantiateFn;

void Script::SetInstantiateFn(PrefabInstantiateFn fn) { s_instantiateFn = std::move(fn); }

bool Script::InvokePrefabInstantiate(Scene& scene, const std::string& path, std::vector<EntityID>& roots)
{
    if (!s_instantiateFn) return false;
    return s_instantiateFn(scene, path, roots);
}

Script::~Script()
{
    CancelEventSubscriptions();
}

// ── コンテキスト設定 ────────────────────────────────────────────────────────

void Script::SetContext(Scene* scene, GameObject* gameObject)
{
    m_scene      = scene;
    m_gameObject = gameObject;
}

void Script::SyncEnabledState()
{
    // WHY: enabled は public 互換性を維持するため setter 化しない。
    //      その代わり ScriptSystem の同期点で前回値と比較し、変化した瞬間だけ通知する。
    if (!m_enableStateInitialized) {
        m_lastEnabled = enabled;
        m_enableStateInitialized = true;
        if (enabled) OnEnable();
        return;
    }

    if (m_lastEnabled == enabled) return;
    m_lastEnabled = enabled;
    if (enabled)
        OnEnable();
    else
        OnDisable();
}

void Script::SetDeltaTime(float dt)
{
    deltaTime         = dt;
    unscaledDeltaTime = dt;
    time += dt;
    ++frameCount;
}

InvokeHandle Script::Invoke(std::function<void()> fn, float delay)
{
    if (!fn) return {};
    InvokeEntry entry{};
    entry.fn        = std::move(fn);
    entry.remaining = delay > 0.0f ? delay : 0.0f;
    entry.id        = m_nextInvokeId++;
    m_invokes.push_back(std::move(entry));
    return { m_invokes.back().id };
}

InvokeHandle Script::InvokeRepeating(std::function<void()> fn, float delay, float interval)
{
    if (!fn) return {};
    if (interval <= 0.0f) return Invoke(std::move(fn), delay);

    InvokeEntry entry{};
    entry.fn        = std::move(fn);
    entry.remaining = delay > 0.0f ? delay : 0.0f;
    entry.interval  = interval;
    entry.repeating = true;
    entry.id        = m_nextInvokeId++;
    m_invokes.push_back(std::move(entry));
    return { m_invokes.back().id };
}

void Script::CancelInvoke()
{
    if (!m_isTickingInvokes) {
        m_invokes.clear();
        m_frameDelays.clear();
        return;
    }
    for (auto& e : m_invokes)     e.canceled = true;
    for (auto& e : m_frameDelays) e.canceled = true;
}

void Script::CancelInvoke(InvokeHandle handle)
{
    if (!handle.IsValid()) return;
    for (auto& e : m_invokes)
        if (e.id == handle.id) { e.canceled = true; return; }
}

void Script::TickInvokes(float dt)
{
    if (m_invokes.empty()) return;

    m_isTickingInvokes = true;
    const size_t initialCount = m_invokes.size();
    for (size_t i = 0; i < initialCount && i < m_invokes.size(); ++i) {
        auto& entry = m_invokes[i];
        if (entry.canceled) continue;

        entry.remaining -= dt;
        if (entry.remaining > 0.0f) continue;

        auto fn = entry.fn;
        if (entry.repeating)
            entry.remaining += entry.interval;
        else
            entry.canceled = true;

        // WHAT: callback 内から Invoke / CancelInvoke が呼ばれてもよい。
        //      fn はコピーしてから呼び、vector の再配置や canceled 更新の影響を受けないようにする。
        if (fn) fn();
    }
    m_isTickingInvokes = false;

    m_invokes.erase(
        std::remove_if(m_invokes.begin(), m_invokes.end(),
            [](const InvokeEntry& entry) { return entry.canceled; }),
        m_invokes.end());
}

void Script::FrameDelay(uint32_t n, std::function<void()> fn)
{
    if (!fn) return;
    FrameDelayEntry entry{};
    entry.fn = std::move(fn);
    entry.remainingFrames = n;
    m_frameDelays.push_back(std::move(entry));
}

void Script::TickFrameDelays()
{
    if (m_frameDelays.empty()) return;

    for (auto& entry : m_frameDelays) {
        if (entry.canceled) continue;
        if (entry.remainingFrames > 0) {
            --entry.remainingFrames;
            continue;
        }
        auto fn = entry.fn;
        entry.canceled = true;
        if (fn) fn();
    }

    m_frameDelays.erase(
        std::remove_if(m_frameDelays.begin(), m_frameDelays.end(),
            [](const FrameDelayEntry& entry) { return entry.canceled; }),
        m_frameDelays.end());
}

void Script::CancelEventSubscriptions()
{
    for (auto& unsubscribe : m_eventUnsubscribers) {
        if (unsubscribe) unsubscribe(this);
    }
    m_eventUnsubscribers.clear();
}

void Script::SetPhysicsWorld(physics::World* world)
{
    s_physicsWorld = world;
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

void Script::QueueRenderPass(UserRenderPassDesc desc) const
{
    if (m_scene)
        m_scene->QueueUserRenderPass(std::move(desc));
}

const renderer::ShaderDescriptor* Script::GetShaderDescriptor(std::string_view shaderPath) const
{
    if (shaderPath.empty()) return nullptr;
    auto* resources = renderer::ResourceManager::Active();
    if (!resources) return nullptr;

    // WHY: Script は ResourceManager を直接所有しない。ここで解決だけを代行し、
    //      MaterialComponent は渡された Descriptor に従って純粋にバイト列を書き換える。
    const auto handle = resources->LoadShader(std::string(shaderPath));
    if (const auto* shader = resources->Get(handle))
        return &shader->GetDescriptor();
    return nullptr;
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
