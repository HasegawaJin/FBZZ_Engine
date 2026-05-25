// FBZZ Engine
// UIAnimatorSystem.hpp | fbzz::scene
// UIAnimator の Tween を進める System
// 色や位置の補間結果を UIImage / Transform へ書き込む。
// UISystem より前に呼ぶことで描画へ反映させる。
#pragma once

namespace fbzz::scene {
class Scene;

void UIAnimatorSystem(Scene& scene, float deltaTime);

} // namespace fbzz::scene
