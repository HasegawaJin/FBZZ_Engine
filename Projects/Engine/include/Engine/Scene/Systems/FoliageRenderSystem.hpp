// FBZZ Engine
// FoliageRenderSystem.hpp | fbzz::scene
// Terrain 上の大型植生を Species/SubMesh 単位で GPU Instancing 描画する
#pragma once

namespace fbzz::scene {

struct RenderPassContext;

// FoliageComponent を Bake し、モデルの全 SubMesh を対応 Material で描画する。
void FoliageRenderSystem(RenderPassContext& ctx);

} // namespace fbzz::scene
