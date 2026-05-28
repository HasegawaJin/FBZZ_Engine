// FBZZ Engine
// DecalPass.hpp | fbzz::scene
// Deferred デカール描画パスのエントリポイント宣言
#pragma once

namespace fbzz::scene {
struct RenderPassContext;
void ExecuteDecalPass(RenderPassContext& ctx);
} // namespace fbzz::scene
