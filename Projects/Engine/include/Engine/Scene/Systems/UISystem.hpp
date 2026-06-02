// FBZZ Engine
// UISystem.hpp | fbzz::scene
// ランタイム UI の描画とボタン入力処理
// UICanvas / UIImage / UIText / UIButton を走査して DrawCall とヒット状態を作る。
// WorldSpace UI には viewProjection を渡して座標変換する。
#pragma once
#include <Math/Vector2.hpp>
#include <Math/Matrix4.hpp>

namespace fbzz {
namespace renderer {
class IRenderer;
class ResourceManager;
}
namespace scene {
class Scene;
}
}

namespace fbzz::scene {

enum class UIRenderTargetView {
    GameViewport,  // 実ゲーム出力。ScreenSpace / WorldSpace の両方を最終合成として描く。
    SceneViewport, // 3D 編集ビュー。シーン内オブジェクトである WorldSpace Canvas だけ描く。
    CanvasEditor   // UI 専用編集ビュー。3D 空間に属する WorldSpace Canvas は描かない。
};

// viewProjection: WorldSpace canvas 用のカメラ VP 行列。WorldSpace を使わない場合は単位行列でよい。
// targetView: Game / UI Editor など、呼び出し元 Viewport の役割に応じて描画対象 Canvas を絞る。
void UISystem(Scene& scene,
              renderer::IRenderer& renderer,
              renderer::ResourceManager& resources,
              float viewportWidth,
              float viewportHeight,
              math::Vector2 mouseInCanvasSpace,
              bool mousePressed,
              const math::Matrix4& viewProjection = math::Matrix4::Identity(),
              UIRenderTargetView targetView = UIRenderTargetView::GameViewport);

} // namespace fbzz::scene
