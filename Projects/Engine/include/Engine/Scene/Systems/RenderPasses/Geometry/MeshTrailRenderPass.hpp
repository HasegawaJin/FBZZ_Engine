/// @file    MeshTrailRenderPass.hpp
/// @brief   MeshTrailComponent のサンプル更新と Mesh / SkinnedMesh 残像 DrawCall 発行。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once

#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Scene/Systems/RenderPasses/IRenderPass.hpp>

namespace fbzz::scene {

/// IRenderPass 実装 — RenderPipeline::`AddPass<MeshTrailRenderPass>()` で登録する。
class MeshTrailRenderPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

} // namespace fbzz::scene
