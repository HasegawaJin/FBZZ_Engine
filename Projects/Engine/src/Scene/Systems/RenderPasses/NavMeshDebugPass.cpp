// FBZZ Engine
// NavMeshDebugPass.cpp | fbzz::scene
// NavMesh / NavSensor のデバッグ描画パス実装。
// RenderGraph の HDR RT 上で描画することで、テレイン深度との整合性を保ちつつ
// ポストプロセスが適用される前のバッファに書き込む。
#include "DebugPasses.hpp"
#include <Engine/Scene/Systems/RenderPassContext.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/Systems/DebugDrawSystem.hpp>

namespace fbzz::scene {

void ExecuteNavMeshDebugPass(RenderPassContext& ctx)
{
    if (!ctx.settings.showNavMesh && !ctx.settings.showNavSensors) return;

    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetViewProjection());
    if (ctx.settings.showNavMesh)
        NavMeshDebugDrawSystem(ctx.scene, ctx.renderer);
    if (ctx.settings.showNavSensors)
        NavMeshSensorDebugDrawSystem(ctx.scene, ctx.renderer);
    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
