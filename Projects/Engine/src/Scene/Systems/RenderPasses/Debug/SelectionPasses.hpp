// FBZZ Engine
// SelectionPasses.hpp | fbzz::scene
// Selection mask render pass declarations
#pragma once

namespace fbzz::scene {

struct RenderPassContext;

void ExecuteSelectionMaskPass(RenderPassContext& ctx);
void ExecuteSelectionOutlinePass(RenderPassContext& ctx);

} // namespace fbzz::scene
