// FBZZ Engine
// MeshTrailRenderSystem.hpp | fbzz::scene
// MeshTrailComponent のサンプル更新と Mesh / SkinnedMesh 残像 DrawCall 発行
#pragma once

#include <Engine/Scene/Systems/RenderPassContext.hpp>

namespace fbzz::scene {

// ExecuteMeshTrailPass — MeshTrailComponent を持つ GameObject の過去姿勢を半透明で描画する。
// WHY: 通常 Trail のリボン生成とは異なり、元 Mesh を再描画するため独立したパスに分ける。
void ExecuteMeshTrailPass(RenderPassContext& ctx);

} // namespace fbzz::scene
