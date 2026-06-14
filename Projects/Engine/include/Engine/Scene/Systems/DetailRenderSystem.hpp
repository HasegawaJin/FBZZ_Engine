// FBZZ Engine
// DetailRenderSystem.hpp | fbzz::scene
// TerrainDetailComponent を走査して GPU Instancing でオブジェクトを描画するシステム。
// TerrainForward パスの直後に呼ぶことで Terrain と同じ HDR RT / depth buffer を共有する。
//
// 実装ファイル: Projects/Engine/src/Scene/Systems/DetailRenderSystem.cpp
// 設計書: Docs/System/Detail/overview.md
#pragma once

namespace fbzz::scene {

struct RenderPassContext;

// Scene 内の全 TerrainDetailComponent を走査し:
//   1. needsBake が true のエンティティで Bake ロジックを実行する
//   2. カメラ距離によるチャンクフィルタリング
//   3. フラスタムカリング（チャンク AABB vs カメラ視錐台）
//   4. 生き残ったチャンクのレイヤー別 DrawInstanced を Submit する
void DetailRenderSystem(RenderPassContext& ctx);

} // namespace fbzz::scene
