/// @file    ScriptDebugDrawPass.cpp
/// @brief   スクリプトの Gizmo と DebugDraw 要求を HDR バッファへ重ねるパス。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// OnDrawGizmos は DebugDraw::BeginFrame / Flush の区間内で呼ぶ必要がある。
/// 複合プリミティブ (Arrow / Cone) はコマンドキューに載せられず、区間内で直接
/// DebugDraw を叩くため。
#include "DebugPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptComponent.hpp>

namespace fbzz::scene {

void ExecuteScriptDebugDrawPass(RenderPassContext& ctx)
{
    Scene& scene = ctx.scene;

    scene.TickScriptDebugDrawCommands(Time::deltaTime);

    // 申告どおり HDR へ描く。前のパスが残した束縛を当てにしない
    // (RenderBindingGuard で炙り出した暗黙依存)。
    ctx.renderer.SetRenderTarget(ctx.Res().Target("HDR"), ctx.resources);
    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetViewProjection());

    for (EntityID id : scene.GetEntities<ScriptComponent>()) {
        auto* sc = scene.GetComponent<ScriptComponent>(id);
        auto* go = scene.GetGameObject(id);
        // ScriptSystem が Update を止める条件に合わせる。止まったスクリプトの
        // ギズモが最後の値のまま残ると、生きているものと見分けが付かない。
        if (!sc || !go || !go->activeInHierarchy()) continue;
        for (auto& entry : sc->scripts) {
            if (!entry.script || !entry.script->enabled) continue;
            entry.script->SetContext(&scene, go);
            entry.script->gizmo.renderer = &ctx.renderer;
            entry.script->ExecuteCallback(&Script::OnDrawGizmos, "OnDrawGizmos");
            entry.script->gizmo.renderer = nullptr;
        }
    }

    for (const auto& command : scene.GetScriptDebugDrawCommands()) {
        switch (command.type) {
        case ScriptDebugDrawType::Line:
            renderer::DebugDraw::Line(ctx.renderer, command.a, command.b, command.color);
            break;
        case ScriptDebugDrawType::Sphere:
            renderer::DebugDraw::Sphere(ctx.renderer, command.a, command.radius, command.color);
            break;
        case ScriptDebugDrawType::Box:
            renderer::DebugDraw::Box(ctx.renderer, command.a, command.halfExtents, command.color);
            break;
        case ScriptDebugDrawType::Ray:
            renderer::DebugDraw::Line(ctx.renderer, command.a, command.b, command.color);
            break;
        case ScriptDebugDrawType::Arrow:
            // headLength = radius, headRadius = halfExtents.x
            renderer::DebugDraw::Arrow(ctx.renderer, command.a, command.b,
                                       command.radius, command.halfExtents.x, command.color);
            break;
        case ScriptDebugDrawType::Cone:
            // direction = b, height = halfExtents.x, baseRadius = radius
            renderer::DebugDraw::Cone(ctx.renderer, command.a, command.b,
                                      command.halfExtents.x, command.radius, command.color);
            break;
        }
    }
    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
