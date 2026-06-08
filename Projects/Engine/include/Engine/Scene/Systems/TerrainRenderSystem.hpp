// FBZZ Engine
// TerrainRenderSystem.hpp | fbzz::scene
// TerrainComponent を走査してチャンクメッシュを生成・描画するシステム
// WHY: 地形描画に必要な「ハイトマップ → GPU メッシュ変換」「チャンク管理」
//      「フラスタムカリング」はシーン全体をまたぐ横断的関心事であり、
//      Component 内に書くと複数エンティティ間の最適化（チャンクキャッシュ共有等）が
//      困難になる。System に分離することで Component はデータのみに専念できる。
// 実装ファイル: Projects/Engine/src/Scene/Systems/TerrainRenderSystem.cpp
#pragma once

#include <Engine/Renderer/ResourceHandle.hpp>
#include <Math/Frustum.hpp>

namespace fbzz::scene { class Scene; }

namespace fbzz::renderer {
    class IRenderer;
    class Camera;
    class ResourceManager;
    struct ConstantBufferTag;
    struct PipelineStateTag;
    struct RenderSettings;
    struct RenderTargetTag;
    struct ShaderTag;
    struct TextureTag;
}

namespace fbzz::scene {

// シーン内の全 TerrainComponent を走査し、チャンクメッシュを生成・描画する。
//   scene     - TerrainComponent と Transform を含むシーン
//   renderer  - DrawCall を受け付ける描画バックエンド
//   resources - シェーダー・バッファ・テクスチャの生成・解決
//   camera    - フラスタムカリングと WVP 計算に使用
//   settings  - ポストプロセス等のレンダリング設定（nullptr = デフォルト）
void TerrainRenderSystem(
    Scene&                                        scene,
    renderer::IRenderer&                          renderer,
    renderer::ResourceManager&                    resources,
    const renderer::Camera&                       camera,
    renderer::ResourceHandle<renderer::RenderTargetTag> outputRT = {},
    const renderer::RenderSettings*               settings = nullptr,
    renderer::ResourceHandle<renderer::TextureTag> shadowDepthTexture = {},
    renderer::ResourceHandle<renderer::ConstantBufferTag> shadowCB = {},
    renderer::ResourceHandle<renderer::ConstantBufferTag> lightCB = {}
);

// SubmitTerrainShadowCasters — Terrain チャンクを現在のシャドウマップ描画へ提出する。
// WHY: Terrain は通常 MeshRenderer を持たないため、ShadowPass 側の共通 caster 収集に
//      専用の提出口を用意する。ライト種別や atlas / cascade 管理は ShadowPass 側に集約し、
//      TerrainRenderSystem はチャンク生成と DrawCall 化だけに責務を限定する。
void SubmitTerrainShadowCasters(
    Scene&                                        scene,
    renderer::IRenderer&                          renderer,
    renderer::ResourceManager&                    resources,
    const math::Frustum&                          lightFrustum,
    renderer::ResourceHandle<renderer::ShaderTag> shadowShader,
    renderer::ResourceHandle<renderer::PipelineStateTag> pipelineState,
    renderer::ResourceHandle<renderer::ConstantBufferTag> frameCB,
    renderer::ResourceHandle<renderer::ConstantBufferTag> objectCB
);

} // namespace fbzz::scene
