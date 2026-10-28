/// @file    WaterRenderPass.hpp
/// @brief   抽出済み水面の描画と共有ノイズ。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Pipeline/RenderPassContext.hpp>
#include <Graphics/Pipeline/IRenderPass.hpp>
namespace fbzz::renderer {
struct RenderWaterInput;
struct RenderViewResources;
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

/// @brief Ensure the atlas and interface shader resources exist before the view graph declares their targets.
[[nodiscard]] bool WaterMediumPipelineResourcesReady(renderer::ResourceManager& resources);

/// @pre Called once while building each view, before declaring graph targets.
/// @return True when the current view has valid water-medium inputs and resources.
bool PrepareWaterMediumResources(RenderPassContext& ctx, RenderViewResources& viewTargets);

/// @return Horizon の水面と水中アトラスが共有する local XZ 範囲。無効設定なら false。
/// @note カメラの far clip とカメラ周辺の光路を包み、有限面の接続境界を保持する。
/// @see Docs/design/water-surface-quality.md Horizon の水面と水中領域。
[[nodiscard]] bool ResolveWaterHorizonRectangle(const RenderWaterInput& input, const Camera& camera,
    math::Vector2& outMin, math::Vector2& outMax);

void WaterSelectionMaskSystem(RenderPassContext& ctx);

/// @note IRenderPass 実装 — RenderPipeline::`AddPass<WaterRenderPass>()` で登録する。
class WaterRenderPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

/// @note Writes the shared water wave/depth surface data into the view-local RGBA32F atlas.
class WaterMediumAtlasPass final : public IRenderPass {
public:
    std::string_view Name() const override { return "WaterMediumAtlas"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

/// @note Captures the nearest visible water surface using the same mesh and view constants as WaterForward.
class WaterInterfacePass final : public IRenderPass {
public:
    std::string_view Name() const override { return "WaterInterface"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

} /// @note namespace fbzz::renderer
