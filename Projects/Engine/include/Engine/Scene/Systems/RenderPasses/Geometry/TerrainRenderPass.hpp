/// @file    TerrainRenderPass.hpp
/// @brief   TerrainComponent を走査してチャンクメッシュを生成・描画するシステム。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// @note ハイトマップ→GPU メッシュ変換・チャンク管理・フラスタムカリングはシーン全体を
///       またぐ横断的関心事で、Component に書くとチャンクキャッシュ共有等の最適化が困難に
///       なるため System へ分離した。実装は Systems/RenderPasses/Geometry/TerrainRenderPass.cpp。
#pragma once

#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Scene/Systems/RenderPasses/IRenderPass.hpp>
#include <Math/Frustum.hpp>

namespace fbzz::scene { class Scene; }

namespace fbzz::renderer {
    class IRenderer;
    class ResourceManager;
    struct ConstantBufferTag;
    struct PipelineStateTag;
    struct ShaderTag;
}

namespace fbzz::scene {

struct RenderPassContext;

/// SubmitTerrainShadowCasters — Terrain チャンクを現在のシャドウマップ描画へ提出する。
/// @note Terrain は通常 MeshRenderer を持たないため専用の提出口を用意する。ライト種別や
///       atlas/cascade 管理は ShadowPass 側に集約し、TerrainRenderPass はチャンク生成と
///       DrawCall 化だけに責務を限定する。シャドウ描画統計の集計のため RenderPassContext
///       ごと受け取る。
void SubmitTerrainShadowCasters(
    RenderPassContext&                            ctx,
    const math::Frustum&                          lightFrustum,
    renderer::ResourceHandle<renderer::ShaderTag> shadowShader,
    renderer::ResourceHandle<renderer::PipelineStateTag> pipelineState,
    renderer::ResourceHandle<renderer::ConstantBufferTag> frameCB,
    renderer::ResourceHandle<renderer::ConstantBufferTag> objectCB
);

void TerrainSelectionMaskSystem(RenderPassContext& ctx);

/// 地形を «どこへ» 描くか。描き先とシェーダーだけが違い、収集と描画の手順は同じ。
/// @note ctx.isDeferred で分けないのは、以前は Forward の GBuffer プリパスの間だけ
///       isDeferred を立てて地形を «入れ子で» Execute しており、その経路では Setup が
///       呼ばれず申告をホスト側のラムダが代理していたため。描き先はパス自身の属性として
///       登録時に決めて持たせる。
enum class TerrainDrawMode {
    GBuffer,  ///< GBuffer へ書く (Deferred 本体 / Forward のプリパス)
    Forward,  ///< HDR へ直接ライティング結果を描く
};

/// IRenderPass 実装 — RenderPipeline::`AddPass<TerrainRenderPass>(mode)` で登録する。
class TerrainRenderPass final : public IRenderPass {
public:
    explicit TerrainRenderPass(TerrainDrawMode mode) : m_mode(mode) {}

    std::string_view Name() const override;
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;

private:
    TerrainDrawMode m_mode;
};

} // namespace fbzz::scene
