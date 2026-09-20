/// @file    TerrainRenderPass.hpp
/// @brief   抽出済み地形の色・影・選択描画。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Pipeline/RenderPassContext.hpp>
#include <Graphics/Pipeline/IRenderPass.hpp>
namespace fbzz::renderer {
void SubmitTerrainShadowCasters(
    RenderPassContext&                            ctx,
    const math::Frustum&                          lightFrustum,
    renderer::ResourceHandle<renderer::ShaderTag> shadowShader,
    renderer::ResourceHandle<renderer::PipelineStateTag> pipelineState,
    renderer::ResourceHandle<renderer::ConstantBufferTag> frameCB,
    renderer::ResourceHandle<renderer::ConstantBufferTag> objectCB
);

void TerrainSelectionMaskSystem(RenderPassContext& ctx);

/// @note 地形を «どこへ» 描くか。描き先とシェーダーだけが違い、収集と描画の手順は同じ。
/// @note ctx.isDeferred で分けないのは、以前は Forward の GBuffer プリパスの間だけ
/// @note isDeferred を立てて地形を «入れ子で» Execute しており、その経路では Setup が
/// @note 呼ばれず申告をホスト側のラムダが代理していたため。描き先はパス自身の属性として
/// @note 登録時に決めて持たせる。
enum class TerrainDrawMode {
    GBuffer,  ///< @note GBuffer へ書く (Deferred 本体 / Forward のプリパス)
    Forward,  ///< @note HDR へ直接ライティング結果を描く
};

/// @note IRenderPass 実装 — RenderPipeline::`AddPass<TerrainRenderPass>(mode)` で登録する。
class TerrainRenderPass final : public IRenderPass {
public:
    explicit TerrainRenderPass(TerrainDrawMode mode) : m_mode(mode) {}

    std::string_view Name() const override;
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;

private:
    TerrainDrawMode m_mode;
};

}
