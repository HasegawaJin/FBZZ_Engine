// FBZZ Engine
// ScriptSystem.cpp | fbzz::scene
// Executes user script lifecycle callbacks
#include "engine/Scene/Systems/ScriptSystem.hpp"
#include "engine/Scene/Scene.hpp"
#include "engine/Scene/GameObject.hpp"
#include "engine/Scene/ScriptComponent.hpp"

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
