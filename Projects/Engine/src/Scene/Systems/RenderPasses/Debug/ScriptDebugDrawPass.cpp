/// @file    ScriptDebugDrawPass.cpp
/// @brief   スクリプトの Gizmo と debug.Draw* 要求を記録し、深度あり / なしの層へ分けて描く。
/// @author  Hasegawa Jin
/// @date    2026-09-10
#include "DebugPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Scene/Systems/RenderSystem.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Core/Profiler/ProfileScope.hpp>
#include <algorithm>
#include <vector>
#include <string>

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

} /// @note namespace

void CaptureScriptGizmos(RenderPassContext& ctx, renderer::DebugDrawCapture& out)
{
    FBZZ_PROFILE_SCOPE("RenderSystem::ScriptGizmos");
    out.Clear();
    const auto spatialCommands = ctx.uiOptions
        ? ctx.uiOptions->spatialDebugCommands : std::span<const ScriptDebugDrawCommand>{};
    if (!ctx.settings.showScriptGizmos && spatialCommands.empty()) return;

    Scene& scene = ctx.scene;
    renderer::DebugDraw::BeginCapture(out);

    if (ctx.settings.showScriptGizmos) {
        struct Target { EntityID entity; std::string entry; std::string instance; bool selected; };
        std::vector<Target> targets;
        for (const EntityID id : scene.GetEntities<ScriptComponent>()) {
            auto* sc = scene.GetComponent<ScriptComponent>(id);
            auto* go = scene.GetGameObject(id);
            /// @note ScriptSystem が Update を止める条件に合わせる。止まったギズモが残ると生死が読めない。
            if (!sc || !go || !go->activeInHierarchy()) continue;
            const bool selected = IsSelectedForDebug(*go, ctx);
            for (const auto& entry : sc->scripts)
                if (entry.script && entry.script->enabled && !entry.m_pendingRemoval)
                    targets.push_back({id, entry.inspectionId, entry.script->InspectionId(), selected});
        }
        const auto resolve = [&](const Target& target, bool runnable = true) -> Script* {
            GameObject* owner = scene.GetGameObject(target.entity);
            auto* scripts = scene.GetComponent<ScriptComponent>(target.entity);
            if (!owner || !scripts || (runnable && !owner->activeInHierarchy())) return nullptr;
            for (const auto& entry : scripts->scripts)
                if (entry.inspectionId == target.entry && entry.script && entry.script->InspectionId() == target.instance
                    && (!runnable || (entry.script->enabled && !entry.m_pendingRemoval && !entry.script->IsRuntimeFaulted()))) {
                    entry.script->SetContext(&scene, owner);
                    return entry.script.get();
                }
            return nullptr;
        };
        /// @note callback を越えて Script/Entry pointer を保持せず、追加は次描画 snapshot から参加する。
        for (const auto& target : targets) {
            if (Script* script = resolve(target)) {
                script->gizmo.renderer = &ctx.renderer;
                script->ExecuteProfiledCallback(&Script::OnDrawGizmos, ScriptCallbackKind::DRAW_GIZMOS, "OnDrawGizmos");
            }
            renderer::DebugDraw::SetDepthTest(false);
            if (target.selected) if (Script* script = resolve(target)) {
                script->gizmo.renderer = &ctx.renderer;
                script->ExecuteProfiledCallback(&Script::OnDrawGizmosSelected, ScriptCallbackKind::DRAW_GIZMOS_SELECTED, "OnDrawGizmosSelected");
            }
            renderer::DebugDraw::SetDepthTest(false);
            if (Script* script = resolve(target, false)) script->gizmo.renderer = nullptr;
        }

        /// @note OnDrawGizmos 内の debug.Draw* もここで拾うため、キューはコールバックの後に読む。
        for (const auto& command : scene.GetScriptDebugDrawCommands())
            DrawQueuedCommand(ctx.renderer, command);
    }

    constexpr std::size_t MAX_SPATIAL_COMMANDS = 8192;
    for (const auto& command : spatialCommands.first((std::min)(spatialCommands.size(), MAX_SPATIAL_COMMANDS)))
        DrawQueuedCommand(ctx.renderer, command);

    renderer::DebugDraw::EndCapture();
}

bool ScriptGizmoPass::IsEnabled(const RenderPassContext&) const
{
    return m_layer == renderer::DebugDrawLayer::DepthTested
        ? !(m_capture.depthLines.empty() && m_capture.triangles.empty())
        : !m_capture.overlayLines.empty();
}

void ScriptGizmoPass::Execute(PassResources&, RenderPassContext& ctx)
{
    /// @note 深度付きは TAA 前の HDR、Overlay は TAA 後の画素中心へ描く。
    const auto viewProjection = m_layer == renderer::DebugDrawLayer::DepthTested
        ? MakeJitteredViewProjection(ctx.camera, ctx.taaJitterNdcX, ctx.taaJitterNdcY)
        : ctx.camera.GetGpuViewProjection();
    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, viewProjection);
    renderer::DebugDraw::Replay(ctx.renderer, m_capture, m_layer);
    renderer::DebugDraw::Flush();
}

} /// @note namespace fbzz::scene
