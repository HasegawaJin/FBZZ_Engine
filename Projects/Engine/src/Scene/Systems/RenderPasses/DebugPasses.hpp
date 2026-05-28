// FBZZ Engine
// DebugPasses.hpp | fbzz::scene
// Debug render pass entry points
#pragma once

namespace fbzz::scene {

struct RenderPassContext;

void ExecuteDebugCollidersPass(RenderPassContext& ctx);
void ExecuteDecalDebugPass(RenderPassContext& ctx);

} // namespace fbzz::scene

