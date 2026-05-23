// FBZZ Engine
// UIAnimatorSystem.hpp | fbzz::scene
// Advances UIAnimator tweens and writes results into UIImage each frame.
// Call before UISystem each frame.
#pragma once

namespace fbzz::scene {
class Scene;

void UIAnimatorSystem(Scene& scene, float deltaTime);

} // namespace fbzz::scene
