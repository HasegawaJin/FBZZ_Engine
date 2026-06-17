// FBZZ Engine
// DetailRenderPass.hpp | fbzz::scene
// TerrainDetailComponent を走査して GPU Instancing でオブジェクトを描画するシステム。
// TerrainForward パスの直後に呼ぶことで Terrain と同じ HDR RT / depth buffer を共有する。
//
// 実装ファイル: Projects/Engine/src/Scene/Systems/RenderPasses/Geometry/DetailRenderPass.cpp
// 設計書: Docs/System/Detail/overview.md
#pragma once

#include <Engine/Scene/Systems/RenderPasses/IRenderPass.hpp>

namespace fbzz::scene {

struct RenderPassContext;

// IRenderPass 実装 — RenderPipeline::AddPass<DetailRenderPass>() で登録する。
class DetailRenderPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    std::vector<renderer::RenderGraph::ResourceAccess> DeclareAccesses(
        const RenderPassContext& ctx) const override;
    void Execute(RenderPassContext& ctx) override;
};

} // namespace fbzz::scene
