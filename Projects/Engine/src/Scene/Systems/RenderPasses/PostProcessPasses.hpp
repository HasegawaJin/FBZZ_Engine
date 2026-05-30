// FBZZ Engine
// PostProcessPasses.hpp | fbzz::scene
// Post-process render pass entry points
#pragma once
#include <cstdint>

namespace fbzz::scene {

struct RenderPassContext;

void ExecuteBloomPass(RenderPassContext& ctx);
void ExecuteSSAOPass(RenderPassContext& ctx);
void ExecuteCompositePass(RenderPassContext& ctx);
void ExecuteSelectionOutlinePass(RenderPassContext& ctx);
void ExecuteFxaaPass(RenderPassContext& ctx);
void ExecuteCustomPostProcessPass(RenderPassContext& ctx, uint32_t customIndex, uint32_t outputIndex);

} // namespace fbzz::scene

