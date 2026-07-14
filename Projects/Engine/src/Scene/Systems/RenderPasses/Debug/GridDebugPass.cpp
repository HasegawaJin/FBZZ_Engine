// FBZZ Engine
// GridDebugPass.cpp | fbzz::scene
// 世界原点を中心とした XZ グリッドを HDR バッファへ描画する IRenderPass 実装
#include "DebugPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/DebugDraw.hpp>

namespace fbzz::scene {

std::string_view GridDebugPass::Name() const { return "GridDebug"; }

std::vector<renderer::RenderGraph::ResourceAccess> GridDebugPass::DeclareAccesses(const RenderPassContext&) const
{
    using U = renderer::RenderGraph::ResourceUsage;
    return { { "HDR", U::ReadWrite } };
}

void GridDebugPass::Execute(RenderPassContext& ctx)
{
    if (!ctx.settings.showGrid) return;

    constexpr float         kCellSize   = 1.0f;
    constexpr int           kHalfCount  = 20;
    constexpr math::Vector4 kGridColor  = { 0.28f, 0.28f, 0.28f, 1.0f };
    constexpr math::Vector4 kAxisColorX = { 0.60f, 0.18f, 0.18f, 1.0f };
    constexpr math::Vector4 kAxisColorZ = { 0.18f, 0.18f, 0.60f, 1.0f };

    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetViewProjection());
    const float extent = static_cast<float>(kHalfCount) * kCellSize;
    for (int i = -kHalfCount; i <= kHalfCount; ++i) {
        const float offset = static_cast<float>(i) * kCellSize;
        // WHY: グリッドは「世界の床に置かれた線」なので、深度テストなしで描くと
        //      メッシュの手前へ浮いて見え前後関係が壊れる。LineDepthTested で
        //      シーンジオメトリに遮蔽させる (ギズモ類は従来どおり深度なしの Line)。
        // Z 方向の線 (X 軸に平行)
        renderer::DebugDraw::LineDepthTested(ctx.renderer,
            { offset, 0.0f, -extent }, { offset, 0.0f, extent },
            (i == 0) ? kAxisColorX : kGridColor);
        // X 方向の線 (Z 軸に平行)
        renderer::DebugDraw::LineDepthTested(ctx.renderer,
            { -extent, 0.0f, offset }, { extent, 0.0f, offset },
            (i == 0) ? kAxisColorZ : kGridColor);
    }
    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
