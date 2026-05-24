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

// viewProjection: WorldSpace canvas 用のカメラ VP 行列。WorldSpace を使わない場合は単位行列でよい。
void UISystem(Scene& scene,
              renderer::IRenderer& renderer,
              renderer::ResourceManager& resources,
              float viewportWidth,
              float viewportHeight,
              math::Vector2 mouseInCanvasSpace,
              bool mousePressed,
              const math::Matrix4& viewProjection = math::Matrix4::Identity());

} // namespace fbzz::scene
