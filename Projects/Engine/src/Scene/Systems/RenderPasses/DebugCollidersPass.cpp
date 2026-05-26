// FBZZ Engine
// DebugCollidersPass.cpp | fbzz::scene
// Debug collider render pass implementation
#include "DebugPasses.hpp"
#include "RenderPassContext.hpp"
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/Systems/ColliderDebugDrawSystem.hpp>

namespace fbzz::scene {

void ExecuteDebugCollidersPass(RenderPassContext& ctx)
{
    if (!ctx.settings.showColliders) return;

    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetViewProjection());
    ColliderDebugDrawSystem(ctx.scene, ctx.renderer);
    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
