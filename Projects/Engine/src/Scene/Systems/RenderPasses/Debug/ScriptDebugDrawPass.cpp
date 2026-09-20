/// @file    ScriptDebugDrawPass.cpp
/// @brief   スクリプトの Gizmo と debug.Draw* 要求を記録し、深度あり / なしの層へ分けて描く。
/// @author  Hasegawa Jin
/// @date    2026-09-10
#include "DebugPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptComponent.hpp>

namespace fbzz::scene {

namespace {

void DrawQueuedCommand(renderer::IRenderer& r, const ScriptDebugDrawCommand& command)
{
    using renderer::DebugDraw;
    DebugDraw::SetDepthTest(command.depthTest);
    switch (command.type) {
    case ScriptDebugDrawType::Line:
    case ScriptDebugDrawType::Ray:
        DebugDraw::Line(r, command.a, command.b, command.color);
        break;
    case ScriptDebugDrawType::Sphere:
        DebugDraw::Sphere(r, command.a, command.radius, command.color);
        break;
    case ScriptDebugDrawType::Box:
        DebugDraw::Box(r, command.a, command.halfExtents, command.color);
        break;
    case ScriptDebugDrawType::OrientedBox:
        DebugDraw::Box(r, command.a, command.halfExtents, command.rotation, command.color);
        break;
    case ScriptDebugDrawType::Arrow:
        DebugDraw::Arrow(r, command.a, command.b, command.radius, command.halfExtents.x, command.color);
        break;
    case ScriptDebugDrawType::Cone:
        DebugDraw::Cone(r, command.a, command.b, command.halfExtents.x, command.radius, command.color);
        break;
    case ScriptDebugDrawType::Capsule:
        DebugDraw::Capsule(r, command.a, command.radius, command.halfExtents.x, command.rotation, command.color);
        break;
    case ScriptDebugDrawType::Circle:
        DebugDraw::Circle(r, command.a, command.b, command.radius, command.color);
        break;
    case ScriptDebugDrawType::Arc:
        DebugDraw::Arc(r, command.a, command.b, command.halfExtents, command.radius, command.angle, command.color);
        break;
    }
    DebugDraw::SetDepthTest(false);
}

} // namespace

void CaptureScriptGizmos(RenderPassContext& ctx, renderer::DebugDrawCapture& out)
{
    out.Clear();
    if (!ctx.settings.showScriptGizmos) return;

    Scene& scene = ctx.scene;
    renderer::DebugDraw::BeginCapture(out);

    for (EntityID id : scene.GetEntities<ScriptComponent>()) {
        auto* sc = scene.GetComponent<ScriptComponent>(id);
        auto* go = scene.GetGameObject(id);
        /// @note ScriptSystem が Update を止める条件に合わせる。止まったギズモが残ると生死が読めない。
        if (!sc || !go || !go->activeInHierarchy()) continue;
        const bool selected = IsSelectedForDebug(*go, ctx);
        for (auto& entry : sc->scripts) {
            if (!entry.script || !entry.script->enabled) continue;
            entry.script->SetContext(&scene, go);
            entry.script->gizmo.renderer = &ctx.renderer;
            entry.script->ExecuteCallback(&Script::OnDrawGizmos, "OnDrawGizmos");
            renderer::DebugDraw::SetDepthTest(false);
            if (selected) {
                entry.script->ExecuteCallback(&Script::OnDrawGizmosSelected, "OnDrawGizmosSelected");
                renderer::DebugDraw::SetDepthTest(false);
            }
            entry.script->gizmo.renderer = nullptr;
        }
    }

    /// @note OnDrawGizmos 内の debug.Draw* もここで拾うため、キューはコールバックの後に読む。
    for (const auto& command : scene.GetScriptDebugDrawCommands())
        DrawQueuedCommand(ctx.renderer, command);

    renderer::DebugDraw::EndCapture();
}

bool ScriptGizmoPass::IsEnabled(const RenderPassContext& ctx) const
{
    if (!ctx.settings.showScriptGizmos) return false;
    return m_layer == renderer::DebugDrawLayer::DepthTested
        ? !(m_capture.depthLines.empty() && m_capture.triangles.empty())
        : !m_capture.overlayLines.empty();
}

void ScriptGizmoPass::Execute(PassResources&, RenderPassContext& ctx)
{
    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetGpuViewProjection());
    renderer::DebugDraw::Replay(ctx.renderer, m_capture, m_layer);
    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
