/// @file    GridDebugPass.cpp
/// @brief   世界原点を中心とした XZ グリッドを HDR バッファへ描画する IRenderPass 実装。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include "DebugPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/DebugDraw.hpp>

namespace fbzz::scene {

std::string_view GridDebugPass::Name() const { return "GridDebug"; }

void GridDebugPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    /// @note 描き先の束縛はフレームワークが行う (SetAutoTarget)。
    builder.ReadWrite("HDR").SetAutoTarget("HDR");
}

bool GridDebugPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.showGrid;
}

void GridDebugPass::Execute(PassResources&, RenderPassContext& ctx)
{
    constexpr float         kCellSize   = 1.0f;
    constexpr int           kHalfCount  = 20;
    constexpr math::Vector4 kGridColor  = { 0.28f, 0.28f, 0.28f, 1.0f };
    constexpr math::Vector4 kAxisColorX = { 0.60f, 0.18f, 0.18f, 1.0f };
    constexpr math::Vector4 kAxisColorZ = { 0.18f, 0.18f, 0.60f, 1.0f };

    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetGpuViewProjection());
    const float extent = static_cast<float>(kHalfCount) * kCellSize;
    for (int i = -kHalfCount; i <= kHalfCount; ++i) {
        const float offset = static_cast<float>(i) * kCellSize;
        /// @note グリッドは世界の床に置かれた線のため、深度テストなしで描くとメッシュの手前へ浮いて見え前後関係が壊れる。LineDepthTested でシーンジオメトリに遮蔽させる (ギズモ類は深度なしの Line のまま)。
        /// @note Z 方向の線 (X 軸に平行)
        renderer::DebugDraw::LineDepthTested(ctx.renderer,
            { offset, 0.0f, -extent }, { offset, 0.0f, extent },
            (i == 0) ? kAxisColorX : kGridColor);
        /// @note X 方向の線 (Z 軸に平行)
        renderer::DebugDraw::LineDepthTested(ctx.renderer,
            { -extent, 0.0f, offset }, { extent, 0.0f, offset },
            (i == 0) ? kAxisColorZ : kGridColor);
    }
    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
