// FBZZ Engine
// ScriptSystem.cpp | fbzz::scene
// ScriptComponent を走査し、複数 Script の Start / Update を適切な順序で呼ぶ。
// Script の所有は ScriptComponent に残し、System は呼び出しだけを行う。
#include "Engine/Scene/Systems/ScriptSystem.hpp"

#include "Engine/Scene/GameObject.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/ScriptComponent.hpp"
#include <Engine/Core/Logger.hpp>
#include <Engine/Profiler/ProfileScope.hpp>

namespace fbzz::scene {

void ScriptSystem(Scene& scene, float dt)
{
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
                s->OnAwake();
                sc->scripts[i].m_awoken = true;
            }

            if (!sc->scripts[i].m_started) {
                FBZZ_LOG_DEBUG("ScriptSystem: OnStart  [%s]", s->GetTypeName());
                s->OnStart();
                sc->scripts[i].m_started = true;
            }

            if (s->enabled) {
                s->TickFrameDelays();
                s->TickInvokes(dt);
                s->OnUpdate();
            }
        }
    }
}

void LateScriptSystem(Scene& scene, float dt)
{
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
                entry.script->OnLateUpdate();
        }
    }
}

} // namespace fbzz::scene
