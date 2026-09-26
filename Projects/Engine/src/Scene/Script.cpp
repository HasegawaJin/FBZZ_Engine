/// @file    Script.cpp
/// @brief   Script 基底クラスの便利 API 実装。
/// @author  Hasegawa Jin
/// @date    2026-05-27
///
/// template 以外のショートハンドをここに集約し、ヘッダの include 依存を最小化する。
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptEvent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/PrefabInstantiate.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Renderer/IShader.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/Uuid.hpp>
#include <algorithm>
#include <cassert>
#include <utility>
#if defined(_MSC_VER)
#include <excpt.h>
#endif

namespace fbzz::scene {

ScriptInspectionIdentity::ScriptInspectionIdentity() : value(util::GenerateUUID()) {}
ScriptInspectionIdentity::ScriptInspectionIdentity(const ScriptInspectionIdentity&)
    : ScriptInspectionIdentity() {}

physics::World*    Script::s_physicsWorld  = nullptr;
bool               Script::s_inPlayMode    = false;
Script::PrefabInstantiateFn Script::s_instantiateFn;

void Script::SetPrefabInstantiationCallback(PrefabInstantiateFn fn)
{
    s_instantiateFn = std::move(fn);
}

bool Script::InstantiatePrefab(Scene& scene, const std::string& path, std::vector<EntityID>& roots)
{
    /// @note コールバックを注入するのは Editor だけなので、未設定時は既定実装 (InstantiatePrefabAsset)
    ///       にフォールバックする。Editor は差分 (override) 付きの生成を注入して上書きする。
    if (!s_instantiateFn) return InstantiatePrefabAsset(scene, path, roots);
    return s_instantiateFn(scene, path, roots);
}

Script::~Script()
{
    CancelEventSubscriptions();
}

/// @name コンテキスト設定

void Script::SetContext(Scene* scene, GameObject* gameObject)
{
    m_scene      = scene;
    m_gameObject = gameObject;
}

namespace {

/// Script の実行時障害を「Editor 全体のクラッシュ」から「当該 Script の停止」へ縮退させる。
/// @note 空の Ref<T> を誤って operator-> で使うと MSVC はアクセス違反を SEH として通知する。C++ 例外
///       ではないため、ここでコールバック境界を保護し、原因を Console へ残す。
void HandleRuntimeFault(fbzz::scene::Script& script, const char* callbackName)
{
    script.enabled = false;
    FBZZ_LOG_ERROR("Script '%s' disabled after an invalid reference/access violation in %s().",
                   script.GetTypeName(), callbackName ? callbackName : "callback");
}

} /// namespace

bool Script::ExecuteCallback(void (Script::*callback)(), const char* callbackName)
{
    if (!callback || m_runtimeFaulted) return false;
#if defined(_MSC_VER)
    __try {
        (this->*callback)();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        m_runtimeFaulted = true;
        HandleRuntimeFault(*this, callbackName);
        return false;
    }
#else
    (this->*callback)();
    return true;
#endif
}

bool Script::ExecuteCallback(void (Script::*callback)(const CollisionInfo&),
                             const CollisionInfo& info,
                             const char* callbackName)
{
    if (!callback || m_runtimeFaulted) return false;
#if defined(_MSC_VER)
    __try {
        (this->*callback)(info);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        m_runtimeFaulted = true;
        HandleRuntimeFault(*this, callbackName);
        return false;
    }
#else
    (this->*callback)(info);
    return true;
#endif
}

bool Script::ExecuteCallback(void (Script::*callback)(const AnimationEventInfo&),
                             const AnimationEventInfo& info)
{
    if (!callback || m_runtimeFaulted) return false;
#if defined(_MSC_VER)
    __try {
        (this->*callback)(info);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        m_runtimeFaulted = true;
        HandleRuntimeFault(*this, "OnAnimationEvent");
        return false;
    }
#else
    (this->*callback)(info);
    return true;
#endif
}

bool Script::ExecuteCallback(void (Script::*callback)(const SequenceEventInfo&),
                             const SequenceEventInfo& info)
{
    if (!callback || m_runtimeFaulted) return false;
#if defined(_MSC_VER)
    __try {
        (this->*callback)(info);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        m_runtimeFaulted = true;
        HandleRuntimeFault(*this, "OnSequenceEvent");
        return false;
    }
#else
    (this->*callback)(info);
    return true;
#endif
}

bool Script::ExecuteCallback(void (Script::*callback)(const char*),
                             const char* argument,
                             const char* callbackName)
{
    if (!callback || m_runtimeFaulted) return false;
#if defined(_MSC_VER)
    __try {
        (this->*callback)(argument);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        m_runtimeFaulted = true;
        HandleRuntimeFault(*this, callbackName);
        return false;
    }
#else
    (this->*callback)(argument);
    return true;
#endif
}

bool Script::ExecuteCallback(void (Script::*callback)(const RootMotionInfo&),
                             const RootMotionInfo& info)
{
    if (!callback || m_runtimeFaulted) return false;
#if defined(_MSC_VER)
    __try {
        (this->*callback)(info);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        m_runtimeFaulted = true;
        HandleRuntimeFault(*this, "OnAnimatorMove");
        return false;
    }
#else
    (this->*callback)(info);
    return true;
#endif
}

bool Script::ExecuteCallback(void (Script::*callback)(RenderPipeline&, RenderPassContext&),
                             RenderPipeline& pipeline,
                             RenderPassContext& context)
{
    if (!callback || m_runtimeFaulted) return false;
#if defined(_MSC_VER)
    __try {
        (this->*callback)(pipeline, context);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        m_runtimeFaulted = true;
        HandleRuntimeFault(*this, "OnSetupRenderPasses");
        return false;
    }
#else
    (this->*callback)(pipeline, context);
    return true;
#endif
}

bool Script::ExecuteCallback(const std::function<void()>& function, const char* callbackName)
{
    if (!function || m_runtimeFaulted) return false;
#if defined(_MSC_VER)
    __try {
        function();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        m_runtimeFaulted = true;
        HandleRuntimeFault(*this, callbackName);
        return false;
    }
#else
    function();
    return true;
#endif
}

bool Script::ResumeCoroutine(Coroutine& coroutine)
{
    if (m_runtimeFaulted) return false;
#if defined(_MSC_VER)
    __try {
        coroutine.Step();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /// @note 巻き戻したフレームは中断点に居らず、以降 done() も destroy() も呼べないため、
        ///       畳まず手放す。畳もうとすると障害を握った意味がなくなる。
        coroutine.Release();
        m_runtimeFaulted = true;
        HandleRuntimeFault(*this, "Coroutine step");
        return false;
    }
#else
    coroutine.Step();
    return true;
#endif
}

void Script::SynchronizeEnabledState(bool gameObjectActive)
{
    /// @note enabled は public 互換性を維持するため setter 化しない。代わりに ScriptSystem の
    ///       同期点で GameObject の有効状態と合わせて比較し、変化した瞬間だけ通知する。
    const bool effectiveEnabled = enabled && gameObjectActive;
    if (!m_enableStateInitialized) {
        m_lastEnabled = effectiveEnabled;
        m_enableStateInitialized = true;
        if (effectiveEnabled) ExecuteCallback(&Script::OnEnable, "OnEnable");
        return;
    }

    if (m_lastEnabled == effectiveEnabled) return;
    m_lastEnabled = effectiveEnabled;
    if (effectiveEnabled)
        ExecuteCallback(&Script::OnEnable, "OnEnable");
    else
        ExecuteCallback(&Script::OnDisable, "OnDisable");
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

void Script::UpdateInvocations(float dt)
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

        /// @note callback 内から Invoke / CancelInvoke が呼ばれてもよいよう、fn はコピーしてから呼び、
        ///       vector の再配置や canceled 更新の影響を受けないようにする。
        if (fn && !ExecuteCallback(fn, "deferred callback"))
            break;
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

void Script::UpdateFrameDelays()
{
    if (m_frameDelays.empty()) return;

    /// @note Swap out so fn() callbacks can safely call FrameDelay() without invalidating our iterator.
    std::vector<FrameDelayEntry> current;
    current.swap(m_frameDelays);

    for (auto& entry : current) {
        if (entry.canceled) continue;
        if (entry.remainingFrames > 0) {
            --entry.remainingFrames;
            m_frameDelays.push_back(std::move(entry));
            continue;
        }
        if (entry.fn && !ExecuteCallback(entry.fn, "FrameDelay callback"))
            break;
    }
}

void Script::StartCoroutine(Coroutine co)
{
    /// @note 即完了した (co_await を一度もしなかった) コルーチンは保持しない
    if (co.Done()) return;
    /// @note Tick 中に開始された場合は再配置を避けるため保留バッファへ積む。
    if (m_isTickingCoroutines)
        m_pendingCoroutines.push_back(std::move(co));
    else
        m_coroutines.push_back(std::move(co));
}

void Script::StopAllCoroutines()
{
    m_pendingCoroutines.clear();
    /// @note コルーチンの中から呼ばれると、今 resume しているハンドル自身を破棄することになる
    ///       ため、ループを抜けてから畳む。
    if (m_isTickingCoroutines) {
        m_stopAllCoroutinesRequested = true;
        return;
    }
    m_coroutines.clear();
}

void Script::UpdateCoroutines()
{
    if (m_coroutines.empty() && m_pendingCoroutines.empty()) return;

    m_isTickingCoroutines = true;
    /// @note 添字ループ。Step 内の再開で StartCoroutine されても追加分は m_pendingCoroutines へ回るため、
    ///       m_coroutines は本ループ中に再確保されない。
    for (size_t i = 0; i < m_coroutines.size(); ++i) {
        if (!ResumeCoroutine(m_coroutines[i])) break;
        if (m_stopAllCoroutinesRequested) break;
    }
    m_isTickingCoroutines = false;

    /// @note 再開したコルーチンは次の中断点まで進んで戻っているため、ここでなら安全に畳める。
    if (m_stopAllCoroutinesRequested) {
        m_stopAllCoroutinesRequested = false;
        m_coroutines.clear();
        m_pendingCoroutines.clear();
        return;
    }

    /// @note 完了したコルーチンを除去する。
    m_coroutines.erase(
        std::remove_if(m_coroutines.begin(), m_coroutines.end(),
            [](const Coroutine& c) { return c.Done(); }),
        m_coroutines.end());

    /// @note ティック中に開始されたコルーチンを取り込む。
    for (auto& c : m_pendingCoroutines)
        m_coroutines.push_back(std::move(c));
    m_pendingCoroutines.clear();
}

void Script::CancelEventSubscriptions()
{
    /// @note ScriptEventBus の購読はオーナー単位で一括解除する。~Script から通るこの経路が、DLL
    ///       ホットリロードで解放されるコードを指すハンドラがバスに残らないことの唯一の保証になる。
    ScriptEventBus::UnsubscribeOwner(this);

    /// @note 個別に登録された解除処理 (将来の別バス用の拡張点)。
    for (auto& unsubscribe : m_eventUnsubscribers) {
        if (unsubscribe) unsubscribe(this);
    }
    m_eventUnsubscribers.clear();
}

void Script::SetPhysicsWorld(physics::World* world)
{
    s_physicsWorld = world;
}

void Script::SetInPlayMode(bool inPlayMode)
{
    s_inPlayMode = inPlayMode;
}

bool Script::IsInPlayMode()
{
    return s_inPlayMode;
}

void Script::ResetLifecycleState()
{
    CancelInvoke();
    StopAllCoroutines();
    CancelEventSubscriptions();
    /// @note 次の OnEnable を「初回」として扱わせる。モードをまたぐ直前に OnDisable を
    ///       出し終えているため、ここを残すと新しいモードの最初の OnEnable が落ちる。
    m_enableStateInitialized = false;
    m_lastEnabled            = true;
    /// @note 障害ラッチは畳んだライフサイクルのもの。次の OnAwake からやり直す。
    /// @note 障害時に落とされた enabled はここでは戻さない。ユーザーが自分で切ったのか障害で
    ///       落ちたのかを区別できず、切ったつもりの Script が復活するため。
    m_runtimeFaulted = false;
}

/// @name シーン操作ショートハンド



/// @name Unity: Object.Destroy


/// @name Animator ショートハンド

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

    /// @note Script は ResourceManager を直接所有しないためここで解決だけを代行し、
    ///       MaterialComponent は渡された Descriptor に従って純粋にバイト列を書き換える。
    const auto handle = resources->LoadShader(std::string(shaderPath));
    if (const auto* shader = resources->Get(handle))
        return &shader->GetDescriptor();
    return nullptr;
}

/// @name PostProcess

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

} /// namespace fbzz::scene
