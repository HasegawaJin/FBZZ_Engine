/// @file    TerrainRenderPass.hpp
/// @brief   TerrainComponent を走査してチャンクメッシュを生成・描画するシステム。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// WHY: 地形描画に必要な「ハイトマップ → GPU メッシュ変換」「チャンク管理」
/// 「フラスタムカリング」はシーン全体をまたぐ横断的関心事であり、
/// Component 内に書くと複数エンティティ間の最適化（チャンクキャッシュ共有等）が
/// 困難になる。System に分離することで Component はデータのみに専念できる。
/// 実装ファイル: Projects/Engine/src/Scene/Systems/RenderPasses/Geometry/TerrainRenderPass.cpp
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

// SubmitTerrainShadowCasters — Terrain チャンクを現在のシャドウマップ描画へ提出する。
// WHY: Terrain は通常 MeshRenderer を持たないため、ShadowPass 側の共通 caster 収集に
//      専用の提出口を用意する。ライト種別や atlas / cascade 管理は ShadowPass 側に集約し、
//      TerrainRenderPass はチャンク生成と DrawCall 化だけに責務を限定する。
// NOTE: シャドウ描画統計を集計するため RenderPassContext ごと受け取る。
void SubmitTerrainShadowCasters(
    RenderPassContext&                            ctx,
    const math::Frustum&                          lightFrustum,
    renderer::ResourceHandle<renderer::ShaderTag> shadowShader,
    renderer::ResourceHandle<renderer::PipelineStateTag> pipelineState,
    renderer::ResourceHandle<renderer::ConstantBufferTag> frameCB,
    renderer::ResourceHandle<renderer::ConstantBufferTag> objectCB
);

void TerrainSelectionMaskSystem(RenderPassContext& ctx);

// IRenderPass 実装 — RenderPipeline::AddPass<TerrainRenderPass>() で登録する。
class TerrainRenderPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    std::vector<renderer::RenderGraph::ResourceAccess> DeclareAccesses(
        const RenderPassContext& ctx) const override;
    void Execute(RenderPassContext& ctx) override;
};

} // namespace fbzz::scene
