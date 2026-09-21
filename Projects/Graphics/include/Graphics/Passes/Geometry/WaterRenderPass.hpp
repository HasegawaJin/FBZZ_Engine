/// @file    WaterRenderPass.hpp
/// @brief   抽出済み水面の描画と共有ノイズ。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Pipeline/RenderPassContext.hpp>
#include <Graphics/Pipeline/IRenderPass.hpp>
namespace fbzz::renderer {
struct WaterDetailNoise {
    renderer::ResourceHandle<renderer::TextureTag> texture;
    /// @note RG を [-1,1] へ戻したあとに掛ける係数。Water.hlsl の g_normalParams.z へ渡す。
    float derivativeScale = 1.0f;
    /// @note 1 / タイル 1 辺のノイズセル数。Water.hlsl の g_timeParams.z へ渡す。
    float invTileCells = 1.0f;
};

/// @note 全水面が共有するさざ波タイルを返す (初回に焼く)。
/// @note マテリアルプレビューも同じタイルを引く。別々に焼くとプレビューと本編で
/// @note さざ波の位相が食い違い、詰めた値がビューポートで再現しない。
/// @see Docs/design/water-waves.md
[[nodiscard]] const WaterDetailNoise& GetWaterDetailNoise(renderer::ResourceManager& resources);

void WaterSelectionMaskSystem(RenderPassContext& ctx);

/// @note IRenderPass 実装 — RenderPipeline::`AddPass<WaterRenderPass>()` で登録する。
class WaterRenderPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

} /// @note namespace fbzz::renderer
