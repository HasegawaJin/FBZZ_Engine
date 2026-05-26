// FBZZ Engine
// PostProcessPasses.hpp | fbzz::scene
// Post-process render pass entry points
#pragma once

namespace fbzz::scene {

struct RenderPassContext;

void ExecuteBloomPass(RenderPassContext& ctx);
void ExecuteCompositePass(RenderPassContext& ctx);
void ExecuteSelectionOutlinePass(RenderPassContext& ctx);
void ExecuteFxaaPass(RenderPassContext& ctx);

} // namespace fbzz::scene

