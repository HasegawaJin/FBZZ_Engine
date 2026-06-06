// FBZZ Engine
// TrailRenderSystem.hpp | fbzz::scene
// TrailComponent を更新し、マイタージョイント付きリボンとして透明描画するシステム
#pragma once

#include <Engine/Scene/Systems/RenderPassContext.hpp>

namespace fbzz::scene {

// ExecuteTrailPass — TrailComponent の制御点更新・頂点展開・DrawCall 発行を行う。
// WHY: Trail は透明描画かつ毎フレーム頂点が変わるため、通常 MeshRenderer ではなく専用パスで扱う。
void ExecuteTrailPass(RenderPassContext& ctx);

} // namespace fbzz::scene
