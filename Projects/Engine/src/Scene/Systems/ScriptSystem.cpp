/// @file    ScriptSystem.cpp
/// @brief   ScriptComponent を走査し、複数 Script の Start / Update を適切な順序で呼ぶ。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// Script の所有は ScriptComponent に残し、System は呼び出しだけを行う。
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
#include <span>
#include <vector>

namespace fbzz::scene {

namespace {

// Play セッション中か。Pause 中も true で、編集中だけ false になる。
//
// WHY playing だけを見ないか: SetPlaying() を呼ばない SceneManager (Standalone の
//     テンプレートやプレビュー用) では playing が既定の false のまま simulating だけ
//     true になる。その構成でも従来どおり全 Script が回るよう、どちらかが立てば Play とみなす。
bool InPlayMode(const SystemContext& ctx)
{
    return ctx.simulating || ctx.playing;
}

// このフレームでスクリプトを一切進めないか (Pause 中)。
bool IsPausedFrame(const SystemContext& ctx)
{
    return ctx.playing && !ctx.simulating;
}

// 走査対象の EntityID を写し取る。
//
// WHY span をそのまま回さないか: GetEntities<T>() は生きたコンポーネント配列への
//     span を返す。スクリプトのコールバックが別の GameObject へ AddScript すると
//     ScriptComponent 配列が再確保され、走査中の span は解放済みの領域を指す。
//     ID だけ先に控えれば、実体はその都度 ID から引き直せる。
std::vector<EntityID> SnapshotScriptEntities(Scene& scene)
{
    const std::span<const EntityID> entities = scene.GetEntities<ScriptComponent>();
    return std::vector<EntityID>(entities.begin(), entities.end());
}

// 初期化済みの Script が、今と違うモードで起こされていたら畳む。
//
// WHY 必要か: Play の開始と停止では Script インスタンスが作り直されない (Stop は
//     スナップショット復元で作り直すが、Play 開始は現物をそのまま走らせる)。
//     編集中に m_awoken / m_started が立ったまま Play へ入ると、OnAwake も OnStart も
//     二度と呼ばれず、初期化を OnStart に置いた Script が Play で沈黙する。
//     モードをまたいだ瞬間に OnDisable → OnDestroy まで通してから作り直しと同じ状態へ戻す。
void CollapseLifecycleAcrossModes(ScriptEntry& entry, Scene& scene, GameObject& go, bool playMode)
{
    if (!entry.m_awoken || entry.m_lifecyclePlayMode == playMode) return;

    Script* s = entry.script.get();
    if (!s) return;

    // WHY コールバックより先にフラグを畳むか: OnDestroy 内の AddScript が
    //     sc->scripts を再確保すると entry の参照が切れる。Script* は heap なので残る。
    entry.m_awoken  = false;
    entry.m_started = false;

    s->SetContext(&scene, &go);
    s->SynchronizeEnabledState(false);
    s->ExecuteCallback(&Script::OnDestroy, "OnDestroy");
    s->ResetLifecycleState();
}

} // namespace

void ScriptSystem::Update(SystemContext& ctx)
{
    Scene& scene = ctx.scene;
    const float dt = ctx.dt;
    FBZZ_PROFILE_SCOPE("ScriptSystem");

    // Pause は「止めている」だけなので、編集中実行の Script も含めて何も進めない。
    if (IsPausedFrame(ctx)) return;

    const bool playMode = InPlayMode(ctx);
    // app.IsPlaying() の実体。編集中も走る Script が自分の居るモードを見分けるために使う。
    Script::SetInPlayMode(playMode);

    for (EntityID id : SnapshotScriptEntities(scene)) {
        auto* sc = scene.GetComponent<ScriptComponent>(id);
        auto* go = scene.GetGameObject(id);
        if (!sc || !go) continue;

        // コールバックをまたいだ後に実体を引き直す。
        // WHY: AddScript が別の GameObject へ ScriptComponent を足すと配列ごと
        //      再確保され、sc は解放済みの領域を指したままになる。i 番目が消えている
        //      こともあるため、範囲もここで見る。
        const auto rebind = [&scene, id, &sc, &go](size_t index) {
            sc = scene.GetComponent<ScriptComponent>(id);
            go = scene.GetGameObject(id);
            return sc && go && index < sc->scripts.size();
        };

        // WHY: index loop + raw Script* because AddScript() inside OnAwake/OnStart/OnUpdate
        //      calls sc->scripts.emplace_back(), potentially reallocating the vector and
        //      invalidating any range-for reference or iterator into sc->scripts.
        //      Script* (heap pointer) remains stable across reallocations.
        const size_t initialCount = sc->scripts.size();
        for (size_t i = 0; i < initialCount; ++i) {
            if (!rebind(i)) break;
            Script* s = sc->scripts[i].script.get();
            if (!s) continue;

            // OnDestroy を通すため、この後は必ず引き直してから sc へ触る。
            CollapseLifecycleAcrossModes(sc->scripts[i], scene, *go, playMode);
            if (!rebind(i)) break;
            if (sc->scripts[i].script.get() != s) continue;

            // WHY ここより前に何も呼ばないか: SetContext / SynchronizeEnabledState は
            //     OnEnable / OnDisable を発火させる。編集中に走らない Script まで通すと、
            //     Play を押していないのにライフサイクルが動き出す。
            if (!playMode && !s->ExecuteInEditMode()) continue;

            s->SetContext(&scene, go);
            const bool gameObjectActive = go->activeInHierarchy();
            s->SynchronizeEnabledState(gameObjectActive);
            if (!rebind(i)) break;

            // WHY: 見た目だけを消して Script を実行すると、非表示のオブジェクトが
            //      Transform / 物理 / スコアなどを裏で更新し続ける。Unity の activeInHierarchy
            //      と同じく、無効な階層ではライフサイクルと毎フレーム処理を止める。
            if (!gameObjectActive)
                continue;

            if (!sc->scripts[i].m_awoken) {
                FBZZ_LOG_DEBUG("ScriptSystem: OnAwake  [%s]", s->GetTypeName());
                s->ExecuteCallback(&Script::OnAwake, "OnAwake");
                if (!rebind(i)) break;
                sc->scripts[i].m_awoken = true;
                sc->scripts[i].m_lifecyclePlayMode = playMode;
            }

            if (!sc->scripts[i].m_started) {
                // FBZZ_REQUIRE_COMPONENT の充足を、そのスクリプトにつき一度だけ検査する。
                // WHY ここ (OnStart の直前) か: 自分の OnAwake で GetOrAddComponent<T>() を
                //     呼んで自前で揃えるスクリプトを誤検知しないよう、OnAwake の後に見る。
                //     エディタは Play 開始前に同じ検証をまとめて出すが、この経路は
                //     Standalone ビルドでも動く最後の防壁になる (実行時追加にも追従する)。
                std::vector<ScriptRequirementIssue> issues;
                CollectScriptRequirementIssues(*go, *s, issues);
                for (const auto& issue : issues)
                    FBZZ_LOG_ERROR("Script requirement: %s",
                                   FormatScriptRequirementIssue(issue).c_str());

                FBZZ_LOG_DEBUG("ScriptSystem: OnStart  [%s]", s->GetTypeName());
                s->ExecuteCallback(&Script::OnStart, "OnStart");
                if (!rebind(i)) break;
                sc->scripts[i].m_started = true;
            }

            if (s->enabled) {
                s->UpdateFrameDelays();
                s->UpdateInvocations(dt);
                s->UpdateCoroutines();
                s->ExecuteCallback(&Script::OnUpdate, "OnUpdate");
            }
        }
    }
}

OrderingHints FixedScriptSystem::GetOrder() const
{
    // スクリプトが加えた力・速度変更を同じステップ内で積分させるため、物理より前。
    return OrderingHints{}.Before<PhysicsSystem>();
}

void FixedScriptSystem::Update(SystemContext& ctx)
{
    Scene& scene = ctx.scene;
    FBZZ_PROFILE_SCOPE("FixedScriptSystem");
    Script::SetInPlayMode(InPlayMode(ctx));

    // OnFixedUpdate 内から time.FixedDeltaTime() で参照できるようにする。
    // WHY ここで書くか: 刻み幅は SystemScheduler の PhaseConfig が持っており、
    //     Time 側からは見えない。固定ステップループに入るこの System が唯一
    //     正しい値を知る場所になる (SetPhysicsHz で変更されても追従する)。
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

        // ScriptSystem と同じ理由で添字ループ + 生ポインタを使う
        // (OnFixedUpdate 内の AddScript による vector 再確保に耐えるため)。
        const size_t initialCount = sc->scripts.size();
        for (size_t i = 0; i < initialCount; ++i) {
            if (!rebind(i)) break;
            Script* s = sc->scripts[i].script.get();
            if (!s) continue;

            // WHY Awake/Start を呼ばないか: 初期化の責務は ScriptSystem 側に一本化する。
            //     同フレームで Phase::Script が先に走るため、ここへ来る時点では
            //     初期化済みが保証される。まだ起きていない Script はこのステップを飛ばす。
            if (!sc->scripts[i].m_started) continue;

            s->SetContext(&scene, go);
            if (s->enabled)
                s->ExecuteCallback(&Script::OnFixedUpdate, "OnFixedUpdate");
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

        // ScriptSystem / FixedScriptSystem と同じ添字ループ。
        // WHY 範囲 for をやめたか (不具合修正): OnLateUpdate 内の AddScript が
        //     sc->scripts を再確保すると、範囲 for が握っている参照とイテレータが
        //     まとめて無効になる。ここだけ規則から外れていた。
        const size_t initialCount = sc->scripts.size();
        for (size_t i = 0; i < initialCount; ++i) {
            if (!rebind(i)) break;
            Script* s = sc->scripts[i].script.get();
            if (!s) continue;

            // FixedScriptSystem と同じ理由で未初期化の Script は飛ばす。
            // WHY 必要か (不具合修正): SynchronizeEnabledState は OnEnable / OnDisable を
            //     発火させる。Phase::Script は無効な階層で OnAwake を呼ばないため、
            //     この判定が無いと OnAwake より先に OnDisable が飛ぶ Script が出る。
            if (!sc->scripts[i].m_started) continue;

            // ScriptSystem と同じ選別。ライフサイクルの張り直しは Phase::Script 側が
            // 同フレームの先で済ませているため、ここでは実行可否だけを見る。
            if (!playMode && !s->ExecuteInEditMode()) continue;

            const bool gameObjectActive = go->activeInHierarchy();
            s->SetContext(&scene, go);
            s->SynchronizeEnabledState(gameObjectActive);
            if (!rebind(i)) break;
            if (sc->scripts[i].script.get() != s) continue;

            if (gameObjectActive && s->enabled)
                s->ExecuteCallback(&Script::OnLateUpdate, "OnLateUpdate");
        }
    }
}

} // namespace fbzz::scene
