// FBZZ Engine
// FoliageRenderPass.hpp | fbzz::scene
// Terrain 上の大型植生を Species/SubMesh 単位で GPU Instancing 描画する
#pragma once

#include <Engine/Scene/Systems/RenderPasses/IRenderPass.hpp>

namespace fbzz::scene {

struct RenderPassContext;

// IRenderPass 実装 — RenderPipeline::AddPass<FoliageRenderPass>() で登録する。
class FoliageRenderPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    std::vector<renderer::RenderGraph::ResourceAccess> DeclareAccesses(
        const RenderPassContext& ctx) const override;
    void Execute(RenderPassContext& ctx) override;
};

} // namespace fbzz::scene
