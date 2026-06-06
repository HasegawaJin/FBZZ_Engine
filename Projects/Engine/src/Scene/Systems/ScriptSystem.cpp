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

        for (auto& entry : sc->scripts) {
            if (!entry.script) continue;

            entry.script->SetContext(&scene, go);
            entry.script->SetDeltaTime(dt);
            entry.script->SyncEnabledState();

            if (!entry.m_awoken) {
                FBZZ_LOG_DEBUG("ScriptSystem: OnAwake  [%s]", entry.script->GetTypeName());
                entry.script->OnAwake();
                entry.m_awoken = true;
            }

            if (!entry.m_started) {
                FBZZ_LOG_DEBUG("ScriptSystem: OnStart  [%s]", entry.script->GetTypeName());
                entry.script->OnStart();
                entry.m_started = true;
            }

            if (entry.script->enabled) {
                entry.script->TickFrameDelays();
                entry.script->TickInvokes(dt);
                entry.script->OnUpdate(dt);
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
                entry.script->OnLateUpdate(dt);
        }
    }
}

} // namespace fbzz::scene
