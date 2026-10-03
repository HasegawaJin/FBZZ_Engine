/// @file    Script.cpp
/// @brief   Script 基底クラスの便利 API 実装。
/// @author  Hasegawa Jin
/// @date    2026-05-27
///
/// @note template 以外のショートハンドをここに集約し、ヘッダの include 依存を最小化する。
#include <Engine/Scene/Script.hpp>
#include <Engine/Profiler/ScriptProfiler.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptEvent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/PrefabInstantiate.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Renderer/IShader.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Audio/VoiceLifetime.hpp>
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
    /// @note        にフォールバックする。Editor は差分 (override) 付きの生成を注入して上書きする。
    if (!s_instantiateFn) return InstantiatePrefabAsset(scene, path, roots);
    return s_instantiateFn(scene, path, roots);
}

Script::~Script()
{
    ScriptProfiler::ForgetInstance(*this);
    ReleaseOwnedAudioLoops();
    CancelEventSubscriptions();
}

/// @name コンテキスト設定

void Script::SetContext(Scene* scene, GameObject* gameObject)
{
    if (m_scene != scene || m_gameObject != gameObject) ReleaseOwnedAudioLoops();
    m_scene      = scene;
    m_gameObject = gameObject;
    m_contextOwner = nullptr;
    ScriptProfiler::RegisterInstance(*this);
}

namespace {

/// @brief Script の実行時障害を「Editor 全体のクラッシュ」から「当該 Script の停止」へ縮退させる。
/// @note 空の Ref<T> を誤って operator-> で使うと MSVC はアクセス違反を SEH として通知する。C++ 例外
/// @note        ではないため、ここでコールバック境界を保護し、原因を Console へ残す。
void HandleRuntimeFault(fbzz::scene::Script& script, const char* callbackName)
{
    script.enabled = false;
    FBZZ_LOG_ERROR("Script '%s' disabled after an invalid reference/access violation in %s().",
                   script.GetTypeName(), callbackName ? callbackName : "callback");
}

/// @note SEH 内には巻き戻しが必要なローカルを置かず、所有と計測は呼び出し側で閉じる。
/// @see https://learn.microsoft.com/en-us/cpp/error-messages/compiler-errors-2/compiler-error-c2712?view=vs-2019 C2712 と SEH 内の unwinding 制約。
/// @see https://learn.microsoft.com/en-us/cpp/build/reference/eh-exception-handling-model /EH の SEH 契約。
template<typename Callback>
bool GuardedInvoke(const Callback& callback)
{
#if defined(_MSC_VER)
    __try { callback(); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    callback();
    return true;
#endif
}

} /// @note namespace

bool Script::ExecuteCallback(void (Script::*callback)(), const char* callbackName)
{
    return ExecuteProfiledCallback(callback, ScriptCallbackKind::UNKNOWN, callbackName);
}

/// @see https://learn.microsoft.com/en-us/cpp/cpp/pointers-to-members?view=msvc-170 メンバー関数ポインターによる仮想ディスパッチ。
bool Script::ExecuteProfiledCallback(void (Script::*callback)(), ScriptCallbackKind kind, const char* callbackName)
{
    if (kind == ScriptCallbackKind::DESTROY || kind == ScriptCallbackKind::DISABLE) ReleaseOwnedAudioLoops();
    if (!callback || m_runtimeFaulted) return false;
    if (m_requirementsBlocked && kind != ScriptCallbackKind::DESTROY && kind != ScriptCallbackKind::DISABLE
        && kind != ScriptCallbackKind::DRAW_GIZMOS && kind != ScriptCallbackKind::DRAW_GIZMOS_SELECTED) return false;
    /// @note 初回起動と再生成時の旧文脈を、新しい OnEnable/OnAwake より先に切り替える。
    if (kind == ScriptCallbackKind::AWAKE || kind == ScriptCallbackKind::ENABLE) ScriptProfiler::RebindInstance(*this);
    const auto invocation = ScriptProfiler::BeginInvocation(*this, kind, callbackName);
    const bool succeeded = GuardedInvoke([&] { (this->*callback)(); });
    ScriptProfiler::EndInvocation(invocation, succeeded);
    if (!succeeded) {
        m_runtimeFaulted = true;
        HandleRuntimeFault(*this, callbackName);
    }
    return succeeded;
}

bool Script::ExecuteCallback(void (Script::*callback)(const CollisionInfo&), const CollisionInfo& info, const char* callbackName)
{
    return ExecuteProfiledCallback(callback, info, ScriptCallbackKind::UNKNOWN, callbackName);
}

bool Script::ExecuteProfiledCallback(void (Script::*callback)(const CollisionInfo&), const CollisionInfo& info, ScriptCallbackKind kind, const char* callbackName)
{
    if (!callback || m_runtimeFaulted || m_requirementsBlocked) return false;
    const auto invocation = ScriptProfiler::BeginInvocation(*this, kind, callbackName);
    const bool succeeded = GuardedInvoke([&] { (this->*callback)(info); });
    ScriptProfiler::EndInvocation(invocation, succeeded);
    if (!succeeded) {
        m_runtimeFaulted = true;
        HandleRuntimeFault(*this, callbackName);
    }
    return succeeded;
}

bool Script::ExecuteCallback(void (Script::*callback)(const AnimationEventInfo&), const AnimationEventInfo& info)
{
    return ExecuteProfiledCallback(callback, info, ScriptCallbackKind::UNKNOWN, "OnAnimationEvent");
}

bool Script::ExecuteProfiledCallback(void (Script::*callback)(const AnimationEventInfo&), const AnimationEventInfo& info, ScriptCallbackKind kind, const char* callbackName)
{
    if (!callback || m_runtimeFaulted || m_requirementsBlocked) return false;
    const auto invocation = ScriptProfiler::BeginInvocation(*this, kind, callbackName);
    const bool succeeded = GuardedInvoke([&] { (this->*callback)(info); });
    ScriptProfiler::EndInvocation(invocation, succeeded);
    if (!succeeded) {
        m_runtimeFaulted = true;
        HandleRuntimeFault(*this, callbackName);
    }
    return succeeded;
}

bool Script::ExecuteCallback(void (Script::*callback)(const SequenceEventInfo&), const SequenceEventInfo& info)
{
    return ExecuteProfiledCallback(callback, info, ScriptCallbackKind::UNKNOWN, "OnSequenceEvent");
}

bool Script::ExecuteProfiledCallback(void (Script::*callback)(const SequenceEventInfo&), const SequenceEventInfo& info, ScriptCallbackKind kind, const char* callbackName)
{
    if (!callback || m_runtimeFaulted || m_requirementsBlocked) return false;
    const auto invocation = ScriptProfiler::BeginInvocation(*this, kind, callbackName);
    const bool succeeded = GuardedInvoke([&] { (this->*callback)(info); });
    ScriptProfiler::EndInvocation(invocation, succeeded);
    if (!succeeded) {
        m_runtimeFaulted = true;
        HandleRuntimeFault(*this, callbackName);
    }
    return succeeded;
}

bool Script::ExecuteCallback(void (Script::*callback)(const char*), const char* argument, const char* callbackName)
{
    return ExecuteProfiledCallback(callback, argument, ScriptCallbackKind::UNKNOWN, callbackName);
}

bool Script::ExecuteProfiledCallback(void (Script::*callback)(const char*), const char* argument, ScriptCallbackKind kind, const char* callbackName)
{
    if (!callback || m_runtimeFaulted || m_requirementsBlocked) return false;
    const auto invocation = ScriptProfiler::BeginInvocation(*this, kind, callbackName);
    const bool succeeded = GuardedInvoke([&] { (this->*callback)(argument); });
    ScriptProfiler::EndInvocation(invocation, succeeded);
    if (!succeeded) {
        m_runtimeFaulted = true;
        HandleRuntimeFault(*this, callbackName);
    }
    return succeeded;
}

bool Script::ExecuteCallback(void (Script::*callback)(const RootMotionInfo&), const RootMotionInfo& info)
{
    return ExecuteProfiledCallback(callback, info, ScriptCallbackKind::UNKNOWN, "OnAnimatorMove");
}

bool Script::ExecuteProfiledCallback(void (Script::*callback)(const RootMotionInfo&), const RootMotionInfo& info, ScriptCallbackKind kind, const char* callbackName)
{
    if (!callback || m_runtimeFaulted || m_requirementsBlocked) return false;
    const auto invocation = ScriptProfiler::BeginInvocation(*this, kind, callbackName);
    const bool succeeded = GuardedInvoke([&] { (this->*callback)(info); });
    ScriptProfiler::EndInvocation(invocation, succeeded);
    if (!succeeded) {
        m_runtimeFaulted = true;
        HandleRuntimeFault(*this, callbackName);
    }
    return succeeded;
}

bool Script::ExecuteCallback(void (Script::*callback)(RenderPipeline&, RenderPassContext&), RenderPipeline& pipeline, RenderPassContext& context)
{
    return ExecuteProfiledCallback(callback, pipeline, context, ScriptCallbackKind::UNKNOWN, "OnSetupRenderPasses");
}

bool Script::ExecuteProfiledCallback(void (Script::*callback)(RenderPipeline&, RenderPassContext&), RenderPipeline& pipeline, RenderPassContext& context, ScriptCallbackKind kind, const char* callbackName)
{
    if (!callback || m_runtimeFaulted || m_requirementsBlocked) return false;
    const auto invocation = ScriptProfiler::BeginInvocation(*this, kind, callbackName);
    const bool succeeded = GuardedInvoke([&] { (this->*callback)(pipeline, context); });
    ScriptProfiler::EndInvocation(invocation, succeeded);
    if (!succeeded) {
        m_runtimeFaulted = true;
        HandleRuntimeFault(*this, callbackName);
    }
    return succeeded;
}

bool Script::ExecuteCallback(const std::function<void()>& function, const char* callbackName)
{
    return ExecuteProfiledCallback(function, ScriptCallbackKind::UNKNOWN, callbackName);
}

bool Script::ExecuteProfiledCallback(const std::function<void()>& function, ScriptCallbackKind kind, const char* callbackName)
{
    if (!function || m_runtimeFaulted) return false;
    const auto invocation = ScriptProfiler::BeginInvocation(*this, kind, callbackName);
    const bool succeeded = GuardedInvoke([&] { function(); });
    ScriptProfiler::EndInvocation(invocation, succeeded);
    if (!succeeded) {
        m_runtimeFaulted = true;
        HandleRuntimeFault(*this, callbackName);
    }
    return succeeded;
}

bool Script::ResumeCoroutine(Coroutine& coroutine)
{
    if (m_runtimeFaulted) return false;
    const auto invocation = ScriptProfiler::BeginInvocation(*this, ScriptCallbackKind::COROUTINE_STEP, "Coroutine step");
    const bool succeeded = GuardedInvoke([&] { coroutine.Step(); });
    ScriptProfiler::EndInvocation(invocation, succeeded);
    if (!succeeded) {
        /// @note SEH で中断したハンドルは done/destroy を呼べないため、以降の所有を手放す。
        coroutine.Release();
        m_runtimeFaulted = true;
        HandleRuntimeFault(*this, "Coroutine step");
    }
    return succeeded;
}

void Script::SynchronizeEnabledState(bool gameObjectActive)
{
    /// @note enabled は public 互換性を維持するため setter 化しない。代わりに ScriptSystem の
    /// @note        同期点で GameObject の有効状態と合わせて比較し、変化した瞬間だけ通知する。
    const bool effectiveEnabled = IsContextEnabled() && gameObjectActive;
    if (!m_enableStateInitialized) {
        m_lastEnabled = effectiveEnabled;
        m_enableStateInitialized = true;
        if (effectiveEnabled) ExecuteProfiledCallback(&Script::OnEnable, ScriptCallbackKind::ENABLE, "OnEnable");
        return;
    }

    if (m_lastEnabled == effectiveEnabled) return;
    m_lastEnabled = effectiveEnabled;
    if (effectiveEnabled)
        ExecuteProfiledCallback(&Script::OnEnable, ScriptCallbackKind::ENABLE, "OnEnable");
    else
        ExecuteProfiledCallback(&Script::OnDisable, ScriptCallbackKind::DISABLE, "OnDisable");
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
        /// @note        vector の再配置や canceled 更新の影響を受けないようにする。
        if (fn && !ExecuteProfiledCallback(fn, ScriptCallbackKind::DEFERRED, "deferred callback"))
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
        if (entry.fn && !ExecuteProfiledCallback(entry.fn, ScriptCallbackKind::FRAME_DELAY, "FrameDelay callback"))
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
    /// @note        ため、ループを抜けてから畳む。
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
    /// @note        m_coroutines は本ループ中に再確保されない。
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
    /// @note        ホットリロードで解放されるコードを指すハンドラがバスに残らないことの唯一の保証になる。
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
    ScriptProfiler::SetExecutionMode(inPlayMode);
    s_inPlayMode = inPlayMode;
}

bool Script::IsInPlayMode()
{
    return s_inPlayMode;
}

void Script::ReleaseOwnedAudioLoops()
{
    /// @note Scene や GameObject を触らないため、Scene 破棄中・DLL 破棄中にも安全に失効できる。
    for (const auto& weak : m_ownedAudioLoops)
        if (const auto lifetime = weak.lock()) lifetime->active = false;
    m_ownedAudioLoops.clear();
}

void Script::ResetLifecycleState()
{
    ScriptProfiler::RebindInstance(*this);
    ReleaseOwnedAudioLoops();
    m_requirementsBlocked = false;
    CancelInvoke();
    StopAllCoroutines();
    CancelEventSubscriptions();
    /// @note 次の OnEnable を「初回」として扱わせる。モードをまたぐ直前に OnDisable を
    /// @note        出し終えているため、ここを残すと新しいモードの最初の OnEnable が落ちる。
    m_enableStateInitialized = false;
    m_lastEnabled            = true;
    /// @note 障害ラッチは畳んだライフサイクルのもの。次の OnAwake からやり直す。
    /// @note 障害時に落とされた enabled はここでは戻さない。ユーザーが自分で切ったのか障害で
    /// @note        落ちたのかを区別できず、切ったつもりの Script が復活するため。
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
    /// @note        MaterialComponent は渡された Descriptor に従って純粋にバイト列を書き換える。
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

} /// @note namespace fbzz::scene
