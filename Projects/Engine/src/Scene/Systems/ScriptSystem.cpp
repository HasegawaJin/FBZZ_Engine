// FBZZ Engine
// ScriptSystem.cpp | fbzz::scene
// ScriptComponent を走査し、複数 Script の Start / Update を適切な順序で呼ぶ。
// Script の所有は ScriptComponent に残し、System は呼び出しだけを行う。
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
#include <vector>

namespace fbzz::scene {

void ScriptSystem::Update(SystemContext& ctx)
{
    Scene& scene = ctx.scene;
    const float dt = ctx.dt;
    FBZZ_PROFILE_SCOPE("ScriptSystem");

    for (EntityID id : scene.GetEntities<ScriptComponent>()) {
        auto* sc = scene.GetComponent<ScriptComponent>(id);
        auto* go = scene.GetGameObject(id);
        if (!sc || !go) continue;

        // WHY: index loop + raw Script* because AddScript() inside OnAwake/OnStart/OnUpdate
        //      calls sc->scripts.emplace_back(), potentially reallocating the vector and
        //      invalidating any range-for reference or iterator into sc->scripts.
        //      Script* (heap pointer) remains stable across reallocations.
        const size_t initialCount = sc->scripts.size();
        for (size_t i = 0; i < initialCount; ++i) {
            Script* s = sc->scripts[i].script.get();
            if (!s) continue;

            s->SetContext(&scene, go);
            s->SyncEnabledState();

            if (!sc->scripts[i].m_awoken) {
                FBZZ_LOG_DEBUG("ScriptSystem: OnAwake  [%s]", s->GetTypeName());
                s->InvokeNoArg(&Script::OnAwake, "OnAwake");
                sc->scripts[i].m_awoken = true;
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
                s->InvokeNoArg(&Script::OnStart, "OnStart");
                sc->scripts[i].m_started = true;
            }

            if (s->enabled) {
                s->TickFrameDelays();
                s->TickInvokes(dt);
                s->TickCoroutines();
                s->InvokeNoArg(&Script::OnUpdate, "OnUpdate");
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

    // OnFixedUpdate 内から time.FixedDeltaTime() で参照できるようにする。
    // WHY ここで書くか: 刻み幅は SystemScheduler の PhaseConfig が持っており、
    //     Time 側からは見えない。固定ステップループに入るこの System が唯一
    //     正しい値を知る場所になる (SetPhysicsHz で変更されても追従する)。
    fbzz::Time::fixedDeltaTime = ctx.fixedDt;

    for (EntityID id : scene.GetEntities<ScriptComponent>()) {
        auto* sc = scene.GetComponent<ScriptComponent>(id);
        auto* go = scene.GetGameObject(id);
        if (!sc || !go) continue;

        // ScriptSystem と同じ理由で添字ループ + 生ポインタを使う
        // (OnFixedUpdate 内の AddScript による vector 再確保に耐えるため)。
        const size_t initialCount = sc->scripts.size();
        for (size_t i = 0; i < initialCount; ++i) {
            Script* s = sc->scripts[i].script.get();
            if (!s) continue;

            // WHY Awake/Start を呼ばないか: 初期化の責務は ScriptSystem 側に一本化する。
            //     同フレームで Phase::Script が先に走るため、ここへ来る時点では
            //     初期化済みが保証される。まだ起きていない Script はこのステップを飛ばす。
            if (!sc->scripts[i].m_started) continue;

            s->SetContext(&scene, go);
            if (s->enabled)
                s->InvokeNoArg(&Script::OnFixedUpdate, "OnFixedUpdate");
        }
    }
}

void LateScriptSystem::Update(SystemContext& ctx)
{
    Scene& scene = ctx.scene;
    FBZZ_PROFILE_SCOPE("LateScriptSystem");

    for (EntityID id : scene.GetEntities<ScriptComponent>()) {
        auto* sc = scene.GetComponent<ScriptComponent>(id);
        auto* go = scene.GetGameObject(id);
        if (!sc || !go) continue;

        for (auto& entry : sc->scripts) {
            if (!entry.script) continue;

            entry.script->SetContext(&scene, go);
            entry.script->SyncEnabledState();
            if (entry.script->enabled)
                entry.script->InvokeNoArg(&Script::OnLateUpdate, "OnLateUpdate");
        }
    }
}

} // namespace fbzz::scene
