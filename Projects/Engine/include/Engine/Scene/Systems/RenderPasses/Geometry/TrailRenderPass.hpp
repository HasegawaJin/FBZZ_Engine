// FBZZ Engine
// TrailRenderSystem.hpp | fbzz::scene
// TrailComponent を更新し、マイタージョイント付きリボンとして透明描画するシステム
#pragma once

#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Scene/Systems/RenderPasses/IRenderPass.hpp>

namespace fbzz::scene {

// IRenderPass 実装 — RenderPipeline::AddPass<TrailRenderPass>() で登録する。
class TrailRenderPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    std::vector<renderer::RenderGraph::ResourceAccess> DeclareAccesses(
        const RenderPassContext& ctx) const override;
    void Execute(RenderPassContext& ctx) override;
};

} // namespace fbzz::scene
