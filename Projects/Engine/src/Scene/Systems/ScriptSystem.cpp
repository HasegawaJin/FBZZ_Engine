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
#include <span>
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
std::vector<EntityID> SnapshotScriptEntities(Scene& scene)
{
    const std::span<const EntityID> entities = scene.GetEntities<ScriptComponent>();
    return std::vector<EntityID>(entities.begin(), entities.end());
}

/// @brief コールバック中の配列移動・削除を跨いで対象を再解決するための識別子。
struct ScriptTarget {
    EntityID entity;
    std::string inspectionId;
    Script* instance;
    bool activeAtSnapshot;
};

std::vector<ScriptTarget> SnapshotScripts(Scene& scene)
{
    std::vector<ScriptTarget> targets;
    for (EntityID id : SnapshotScriptEntities(scene)) {
        auto* go = scene.GetGameObject(id);
        auto* component = scene.GetComponent<ScriptComponent>(id);
        if (!go || !component) continue;
        for (const auto& entry : component->scripts)
            if (entry.script)
                targets.push_back({id, entry.inspectionId, entry.script.get(), go->activeInHierarchy()});
    }
    return targets;
}

ScriptEntry* Resolve(Scene& scene, const ScriptTarget& target)
{
    auto* component = scene.GetComponent<ScriptComponent>(target.entity);
    if (!component) return nullptr;
    for (auto& entry : component->scripts)
        if (entry.inspectionId == target.inspectionId && entry.script.get() == target.instance)
            return &entry;
    return nullptr;
}

ScriptEntry* Runnable(Scene& scene, const ScriptTarget& target, bool playMode)
{
    auto* entry = Resolve(scene, target);
    auto* go = scene.GetGameObject(target.entity);
    if (!entry || !go || !target.activeAtSnapshot || !go->activeInHierarchy()) return nullptr;
    if (!playMode && !entry->script->ExecuteInEditMode()) return nullptr;
    entry->script->SetContext(&scene, go);
    return entry;
}

/// @note モード変更の終了処理を全員分終えてから、新しいモードの Awake を始める。
void Prepare(Scene& scene, const ScriptTarget& target, bool playMode)
{
    auto* entry = Resolve(scene, target);
    auto* go = scene.GetGameObject(target.entity);
    if (!entry || !go) return;
    const bool reset = entry->m_awoken && entry->m_lifecyclePlayMode != playMode;
    if (reset) {
        entry->m_awoken = false;
        entry->m_started = false;
        entry->script->SetContext(&scene, go);
        entry->script->SynchronizeEnabledState(false);
        if (!(entry = Resolve(scene, target))) return;
        entry->script->ExecuteProfiledCallback(&Script::OnDestroy, ScriptCallbackKind::DESTROY, "OnDestroy");
        if (!(entry = Resolve(scene, target))) return;
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
    const auto targets = SnapshotScripts(scene);
    for (const auto& target : targets) Prepare(scene, target, playMode);

    for (const auto& target : targets) {
        auto* entry = Runnable(scene, target, playMode);
        if (!entry || entry->m_awoken) continue;
        entry->m_awoken = true;
        entry->m_lifecyclePlayMode = playMode;
        entry->script->ExecuteProfiledCallback(&Script::OnAwake, ScriptCallbackKind::AWAKE, "OnAwake");
    }

    /// @note 他者の Awake が設定した状態を Start から参照できる。Start 同士の依存は作らない。
    for (const auto& target : targets) {
        auto* entry = Runnable(scene, target, playMode);
        if (!entry || !entry->m_awoken || entry->m_started) continue;
        if (!ValidateScriptRequirementsForStart(*scene.GetGameObject(target.entity), *entry->script)) continue;
        entry->script->ExecuteProfiledCallback(&Script::OnStart, ScriptCallbackKind::START, "OnStart");
        if ((entry = Resolve(scene, target))) entry->m_started = true;
    }

    for (const auto& target : targets) {
        const auto active = [&]() -> Script* {
            auto* entry = Runnable(scene, target, playMode);
            return entry && entry->m_started && !entry->script->RequirementsBlocked() && entry->script->enabled ? entry->script.get() : nullptr;
        };
        /// @note 遅延処理でも自身や他者を無効化・削除できるため、各呼び出し後に引き直す。
        if (auto* script = active()) script->UpdateFrameDelays();
        if (auto* script = active()) script->UpdateInvocations(ctx.dt);
        if (auto* script = active()) script->UpdateCoroutines();
        if (auto* script = active()) script->ExecuteProfiledCallback(&Script::OnUpdate, ScriptCallbackKind::UPDATE, "OnUpdate");
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

    for (EntityID id : SnapshotScriptEntities(scene)) {
        auto* sc = scene.GetComponent<ScriptComponent>(id);
        auto* go = scene.GetGameObject(id);
        if (!sc || !go) continue;
        if (!go->activeInHierarchy()) continue;

        const auto rebind = [&scene, id, &sc, &go](size_t index) {
            sc = scene.GetComponent<ScriptComponent>(id);
            go = scene.GetGameObject(id);
            return sc && go && index < sc->scripts.size();
        };

        /// @note ScriptSystem と同じ理由で添字ループ + 生ポインタを使う
        /// @note       (OnFixedUpdate 内の AddScript による vector 再確保に耐えるため)。
        const size_t initialCount = sc->scripts.size();
        for (size_t i = 0; i < initialCount; ++i) {
            if (!rebind(i)) break;
            Script* s = sc->scripts[i].script.get();
            if (!s) continue;

            /// @note 初期化の責務は ScriptSystem 側に一本化し、Awake/Start はここで呼ばない。
            /// @note       同フレームで Phase::Script が先に走るため、ここへ来る時点では初期化済みが
            /// @note       保証される。まだ起きていない Script はこのステップを飛ばす。
            if (!sc->scripts[i].m_started || s->RequirementsBlocked()) continue;

            s->SetContext(&scene, go);
            if (s->enabled)
                s->ExecuteProfiledCallback(&Script::OnFixedUpdate, ScriptCallbackKind::FIXED_UPDATE, "OnFixedUpdate");
        }
    }
}

void LateScriptSystem::Update(SystemContext& ctx)
{
    Scene& scene = ctx.scene;
    FBZZ_PROFILE_SCOPE("LateScriptSystem");

    if (IsPausedFrame(ctx)) return;

    const bool playMode = InPlayMode(ctx);
    Script::SetInPlayMode(playMode);

    for (EntityID id : SnapshotScriptEntities(scene)) {
        auto* sc = scene.GetComponent<ScriptComponent>(id);
        auto* go = scene.GetGameObject(id);
        if (!sc || !go) continue;

        const auto rebind = [&scene, id, &sc, &go](size_t index) {
            sc = scene.GetComponent<ScriptComponent>(id);
            go = scene.GetGameObject(id);
            return sc && go && index < sc->scripts.size();
        };

        /// @note ScriptSystem / FixedScriptSystem と同じ添字ループ (不具合修正: 範囲 for だと
        /// @note       OnLateUpdate 内の AddScript が sc->scripts を再確保したとき、握っている参照と
        /// @note       イテレータがまとめて無効になる。ここだけ規則から外れていた)。
        const size_t initialCount = sc->scripts.size();
        for (size_t i = 0; i < initialCount; ++i) {
            if (!rebind(i)) break;
            Script* s = sc->scripts[i].script.get();
            if (!s) continue;

            /// @note FixedScriptSystem と同じ理由で未初期化の Script は飛ばす (不具合修正:
            /// @note       SynchronizeEnabledState は OnEnable/OnDisable を発火させる。Phase::Script は
            /// @note       無効な階層で OnAwake を呼ばないため、この判定が無いと OnAwake より先に
            /// @note       OnDisable が飛ぶ Script が出ていた)。
            if (!sc->scripts[i].m_started || s->RequirementsBlocked()) continue;

            /// @note ScriptSystem と同じ選別。ライフサイクルの張り直しは Phase::Script 側が
            /// @note       同フレームの先で済ませているため、ここでは実行可否だけを見る。
            if (!playMode && !s->ExecuteInEditMode()) continue;

            const bool gameObjectActive = go->activeInHierarchy();
            s->SetContext(&scene, go);
            s->SynchronizeEnabledState(gameObjectActive);
            if (!rebind(i)) break;
            if (sc->scripts[i].script.get() != s) continue;

            if (gameObjectActive && s->enabled)
                s->ExecuteProfiledCallback(&Script::OnLateUpdate, ScriptCallbackKind::LATE_UPDATE, "OnLateUpdate");
        }
    }
}

} /// @note namespace fbzz::scene
