// FBZZ Engine
// TerrainRenderSystem.hpp | fbzz::scene
// TerrainComponent を走査してチャンクメッシュを生成・描画するシステム
//
// WHY: 地形描画に必要な「ハイトマップ → GPU メッシュ変換」「チャンク管理」
//      「フラスタムカリング」はシーン全体をまたぐ横断的関心事であり、
//      Component 内に書くと複数エンティティ間の最適化（チャンクキャッシュ共有等）が
//      困難になる。System に分離することで Component はデータのみに専念できる。
//
// 使い方:
//   // エンジンの更新ループ末尾でシーン描画前に呼ぶ
//   fbzz::scene::TerrainRenderSystem(scene, renderer, resources, camera);
//
// 実装ファイル: Projects/Engine/src/Scene/Systems/TerrainRenderSystem.cpp
#pragma once
#include <Engine/Renderer/ResourceHandle.hpp>

namespace fbzz::scene   { class Scene; }
namespace fbzz::renderer {
    class IRenderer;
    class Camera;
    class ResourceManager;
    struct RenderSettings;
}

namespace fbzz::scene {

// シーン内の全 TerrainComponent を走査し、チャンクメッシュを生成・描画する。
//
// 引数:
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
    const renderer::RenderSettings*               settings = nullptr
);

} // namespace fbzz::scene
