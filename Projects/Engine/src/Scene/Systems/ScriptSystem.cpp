/// @file    ScriptSystem.cpp
/// @brief   ScriptComponent を走査し、複数 Script の Start / Update を適切な順序で呼ぶ。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note
/// @note Script の所有は ScriptComponent に残し、System は呼び出しだけを行う。
#include "Engine/Scene/Systems/ScriptSystem.hpp"
#include "Engine/Core/Scheduler/SystemContext.hpp"
#include "Engine/Scene/GameObject.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/ScriptComponent.hpp"
#include "Engine/Scene/ScriptValidation.hpp"
#include "Engine/Scene/Systems/PhysicsSystem.hpp"
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Profiler/ProfileScope.hpp>
#include <Engine/Profiler/ScriptProfiler.hpp>
#include "../Managed/ManagedScriptJobs.hpp"
#include <span>
#include <algorithm>
#include <vector>

namespace fbzz::scene {

namespace {

/// @brief Play セッション中か。Pause 中も true で、編集中だけ false になる。
/// @note SetPlaying() を呼ばない Standalone のテンプレート/プレビューでは playing が false の
/// @note       まま simulating だけ true になる。その構成でも全 Script が回るよう、どちらかが
/// @note       立てば Play とみなす。
bool InPlayMode(const SystemContext& ctx)
{
    return ctx.simulating || ctx.playing;
}

/// @note このフレームでスクリプトを一切進めないか (Pause 中)。
bool IsPausedFrame(const SystemContext& ctx)
{
    return ctx.playing && !ctx.simulating;
}

/// @brief 走査対象の EntityID を写し取る。
/// @note `GetEntities<T>()` は生きたコンポーネント配列への span を返す。コールバックが別の
/// @note       GameObject へ AddScript すると配列が再確保され、span は解放済み領域を指す。ID だけ
/// @note       控えれば、実体はその都度引き直せる。
void SnapshotScriptEntities(Scene& scene, std::vector<EntityID>& snapshot)
{
    const std::span<const EntityID> entities = scene.GetEntities<ScriptComponent>();
    snapshot.assign(entities.begin(), entities.end());
}

/// @brief コールバック中の配列移動・削除を跨いで対象を再解決するための識別子。
struct ScriptTarget {
    EntityID entity;
    size_t inspectionOffset;
    size_t inspectionBytes;
    size_t instanceOffset;
    size_t instanceBytes;
    bool activeAtSnapshot;
    int executionOrder;
};

struct ScriptSnapshotStorage {
    std::vector<EntityID> entities;
    std::vector<ScriptTarget> targets;
    std::vector<char> identities;
};
/// @note Each active or nested dispatch owns its vectors; only cleared value buffers return to thread-local idle storage.
struct ScriptSnapshot : ScriptSnapshotStorage {
    ScriptSnapshot()
    {
        thread_local ScriptSnapshotStorage idle;
        m_idle = &idle;
        entities.swap(idle.entities);
        targets.swap(idle.targets);
        identities.swap(idle.identities);
    }
    ~ScriptSnapshot()
    {
        entities.clear(); targets.clear(); identities.clear();
        if (entities.capacity() > m_idle->entities.capacity()) entities.swap(m_idle->entities);
        if (targets.capacity() > m_idle->targets.capacity()) targets.swap(m_idle->targets);
        if (identities.capacity() > m_idle->identities.capacity()) identities.swap(m_idle->identities);
    }
    ScriptSnapshot(const ScriptSnapshot&) = delete;
    ScriptSnapshot& operator=(const ScriptSnapshot&) = delete;

    size_t CopyIdentity(std::string_view identity)
    {
        const size_t offset = identities.size();
        identities.insert(identities.end(), identity.begin(), identity.end());
        return offset;
    }
    std::string_view Identity(size_t offset, size_t bytes) const
    { return bytes ? std::string_view(identities.data() + offset, bytes) : std::string_view{}; }
private:
    ScriptSnapshotStorage* m_idle = nullptr;
};

void SnapshotScripts(Scene& scene, ScriptSnapshot& snapshot)
{
    SnapshotScriptEntities(scene, snapshot.entities);
    const auto& entities = snapshot.entities;
    snapshot.targets.reserve(entities.size());
    /// @note 二つの通常 UUID の byte 数だけを確保の目安にし、長い ID や複数 Script も動的に伸ばす。
    if (entities.size() <= snapshot.identities.max_size() / 72U)
        snapshot.identities.reserve(entities.size() * 72U);
    for (EntityID id : entities) {
        auto* go = scene.GetGameObject(id);
        auto* component = scene.GetComponent<ScriptComponent>(id);
        if (!go || !component) continue;
        for (const auto& entry : component->scripts)
            if (entry.script) {
                const auto& instanceId = entry.script->InspectionId();
                snapshot.targets.push_back({id, snapshot.CopyIdentity(entry.inspectionId), entry.inspectionId.size(),
                    snapshot.CopyIdentity(instanceId), instanceId.size(), go->activeInHierarchy(), entry.script->ExecutionOrder()});
            }
    }
    const auto byExecutionOrder = [](const ScriptTarget& left, const ScriptTarget& right) {
        return left.executionOrder < right.executionOrder;
    };
    if (!std::is_sorted(snapshot.targets.begin(), snapshot.targets.end(), byExecutionOrder))
        std::stable_sort(snapshot.targets.begin(), snapshot.targets.end(), byExecutionOrder);
}

ScriptEntry* Resolve(Scene& scene, const ScriptSnapshot& snapshot, const ScriptTarget& target)
{
    auto* component = scene.GetComponent<ScriptComponent>(target.entity);
    if (!component) return nullptr;
    for (auto& entry : component->scripts)
        if (entry.inspectionId == snapshot.Identity(target.inspectionOffset, target.inspectionBytes)
            && entry.script && entry.script->InspectionId() == snapshot.Identity(target.instanceOffset, target.instanceBytes)
            && !entry.m_pendingRemoval)
            return &entry;
    return nullptr;
}

ScriptEntry* Runnable(Scene& scene, const ScriptSnapshot& snapshot, const ScriptTarget& target, bool playMode)
{
    auto* entry = Resolve(scene, snapshot, target);
    auto* go = scene.GetGameObject(target.entity);
    if (!entry || !go || !target.activeAtSnapshot || !go->activeInHierarchy()) return nullptr;
    if (!playMode && !entry->script->ExecuteInEditMode()) return nullptr;
    entry->script->SetContext(&scene, go);
    return entry;
}

/// @note モード変更の終了処理を全員分終えてから、新しいモードの Awake を始める。
void Prepare(Scene& scene, const ScriptSnapshot& snapshot, const ScriptTarget& target, bool playMode)
{
    auto* entry = Resolve(scene, snapshot, target);
    auto* go = scene.GetGameObject(target.entity);
    if (!entry || !go) return;
    const bool reset = entry->m_awoken && entry->m_lifecyclePlayMode != playMode;
    if (reset) {
        entry->m_awoken = false;
        entry->m_started = false;
        entry->script->SetContext(&scene, go);
        entry->script->SynchronizeEnabledState(false);
        if (!(entry = Resolve(scene, snapshot, target))) return;
        entry->script->ExecuteProfiledCallback(&Script::OnDestroy, ScriptCallbackKind::DESTROY, "OnDestroy");
        if (!(entry = Resolve(scene, snapshot, target))) return;
        entry->script->ResetLifecycleState();
    }
    if (!playMode && !entry->script->ExecuteInEditMode()) return;
    go = scene.GetGameObject(target.entity);
    if (!go) return;
    entry->script->SetContext(&scene, go);
    entry->script->SynchronizeEnabledState(go->activeInHierarchy());
}

} /// @note namespace

void ScriptSystem::Update(SystemContext& ctx)
{
    Scene& scene = ctx.scene;
    FBZZ_PROFILE_SCOPE("ScriptSystem");
    const bool playMode = InPlayMode(ctx);
    Script::SetInPlayMode(playMode);
    ScriptProfiler::ObserveScene(scene);
    if (IsPausedFrame(ctx)) return;

    /// @note フレーム中の追加は次回から参加する。生成を繰り返す Awake でも必ず有限で終える。
    ScriptSnapshot snapshot;
    {
        FBZZ_PROFILE_SCOPE("ScriptSystem::Snapshot");
        SnapshotScripts(scene, snapshot);
    }
    {
        FBZZ_PROFILE_SCOPE("ScriptSystem::Prepare");
        for (const auto& target : snapshot.targets) Prepare(scene, snapshot, target, playMode);
    }
    if (playMode) {
        FBZZ_PROFILE_SCOPE("ScriptSystem::OwnedJobs");
        managed_detail::TickManagedJobs(scene, ctx.dt, Time::unscaledDeltaTime);
    }

    {
        FBZZ_PROFILE_SCOPE("ScriptSystem::AwakeStart");
        for (const auto& target : snapshot.targets) {
            auto* entry = Runnable(scene, snapshot, target, playMode);
            if (!entry || entry->m_awoken) continue;
            entry->m_awoken = true;
            entry->m_lifecyclePlayMode = playMode;
            entry->script->ExecuteProfiledCallback(&Script::OnAwake, ScriptCallbackKind::AWAKE, "OnAwake");
        }

        /// @note 他者の Awake が設定した状態を Start から参照できる。Start 同士の依存は作らない。
        for (const auto& target : snapshot.targets) {
            auto* entry = Runnable(scene, snapshot, target, playMode);
            if (!entry || !entry->m_awoken || entry->m_started) continue;
            if (!ValidateScriptRequirementsForStart(*scene.GetGameObject(target.entity), *entry->script)) continue;
            entry->script->ExecuteProfiledCallback(&Script::OnStart, ScriptCallbackKind::START, "OnStart");
            if ((entry = Resolve(scene, snapshot, target))) entry->m_started = true;
        }
    }

    {
        FBZZ_PROFILE_SCOPE("ScriptSystem::UpdateDispatch");
        for (const auto& target : snapshot.targets) {
            const auto active = [&]() -> Script* {
                auto* entry = Runnable(scene, snapshot, target, playMode);
                return entry && entry->m_started && !entry->script->RequirementsBlocked() && entry->script->enabled ? entry->script.get() : nullptr;
            };
            /// @note 遅延処理でも自身や他者を無効化・削除できるため、各呼び出し後に引き直す。
            auto* script = active();
            /// @note Once a gate rejects the target, no callback runs that could make a later gate accept it in this target's turn.
            if (!script) continue;
            script->UpdateFrameDelays();
            if (!(script = active())) continue;
            script->UpdateInvocations(ctx.dt);
            if (!(script = active())) continue;
            script->UpdateCoroutines();
            if ((script = active())) script->ExecuteProfiledCallback(&Script::OnUpdate, ScriptCallbackKind::UPDATE, "OnUpdate");
        }
    }
}

OrderingHints FixedScriptSystem::GetOrder() const
{
    /// @note スクリプトが加えた力・速度変更を同じステップ内で積分させるため、物理より前。
    return OrderingHints{}.Before<PhysicsSystem>();
}

void FixedScriptSystem::Update(SystemContext& ctx)
{
    Scene& scene = ctx.scene;
    FBZZ_PROFILE_SCOPE("FixedScriptSystem");
    Script::SetInPlayMode(InPlayMode(ctx));

    /// @note OnFixedUpdate 内から time.FixedDeltaTime() で参照できるようにする。刻み幅は
    /// @note       SystemScheduler の PhaseConfig が持っており Time 側からは見えないため、固定
    /// @note       ステップループに入るこの System が唯一正しい値を知る場所になる。
    fbzz::Time::fixedDeltaTime = ctx.fixedDt;

    const bool playMode = InPlayMode(ctx);
    if (!playMode || IsPausedFrame(ctx)) return;
    ScriptSnapshot snapshot;
    SnapshotScripts(scene, snapshot);
    for (const auto& target : snapshot.targets) {
        auto* entry = Runnable(scene, snapshot, target, playMode);
        if (!entry || !entry->m_started || entry->script->RequirementsBlocked() || !entry->script->enabled) continue;
        /// @note 初期化は Phase::Script が担い、この固定 step では開始済みの snapshot 個体だけを配送する。
        entry->script->ExecuteProfiledCallback(&Script::OnFixedUpdate, ScriptCallbackKind::FIXED_UPDATE, "OnFixedUpdate");
    }
}

void LateScriptSystem::Update(SystemContext& ctx)
{
    Scene& scene = ctx.scene;
    /// @note Scheduler の外側 LateScriptSystem と配送区間を別名にし、Flat の同名 Inclusive 合算を避ける。
    FBZZ_PROFILE_SCOPE("LateScriptSystem::Dispatch");

    if (IsPausedFrame(ctx)) return;

    const bool playMode = InPlayMode(ctx);
    Script::SetInPlayMode(playMode);

    ScriptSnapshot snapshot;
    SnapshotScripts(scene, snapshot);
    for (const auto& target : snapshot.targets) {
        auto* entry = Resolve(scene, snapshot, target);
        auto* go = scene.GetGameObject(target.entity);
        if (!entry || !go || !entry->m_started || entry->script->RequirementsBlocked()) continue;
        if (!playMode && !entry->script->ExecuteInEditMode()) continue;
        entry->script->SetContext(&scene, go);
        entry->script->SynchronizeEnabledState(go->activeInHierarchy());
        /// @note OnEnable / OnDisable の構成変更を越え、entry の参照を再取得してから配送する。
        entry = Runnable(scene, snapshot, target, playMode);
        if (entry && entry->m_started && !entry->script->RequirementsBlocked() && entry->script->enabled)
            entry->script->ExecuteProfiledCallback(&Script::OnLateUpdate, ScriptCallbackKind::LATE_UPDATE, "OnLateUpdate");
    }
}

} /// @note namespace fbzz::scene
