// FBZZ Engine
// ScriptSystem.cpp | fbzz::scene
// ユーザースクリプトのライフサイクル実行
// ScriptComponent を走査し、Start / Update を適切な順序で呼ぶ。
// Script の所有は Component 側に残し、System は呼び出しだけを行う。
#include "Engine/Scene/Systems/ScriptSystem.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/GameObject.hpp"
#include "Engine/Scene/ScriptComponent.hpp"

namespace fbzz::scene {

void ScriptSystem(Scene& scene, float dt)
{
    for (EntityID id : scene.GetEntities<ScriptComponent>()) {
        auto* sc = scene.GetComponent<ScriptComponent>(id);
        auto* go = scene.GetGameObject(id);
        if (!sc || !sc->script || !go) continue;

        if (!sc->m_started) {
            sc->script->SetContext(&scene, go);
            sc->script->OnStart();
            sc->m_started = true;
        }

        if (sc->script->enabled)
            sc->script->OnUpdate(dt);
    }
}

} // namespace fbzz::scene
