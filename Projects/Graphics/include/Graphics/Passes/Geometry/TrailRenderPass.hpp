/// @file    TrailRenderPass.hpp
/// @brief   TrailComponent を更新し、マイタージョイント付きリボンとして透明描画するシステム。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once

#include <Graphics/Pipeline/RenderPassContext.hpp>
#include <Graphics/Pipeline/IRenderPass.hpp>

namespace fbzz::renderer {

/// @note IRenderPass 実装 — RenderPipeline::`AddPass<TrailRenderPass>()` で登録する。
class TrailRenderPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

} /// @note namespace fbzz::renderer
