// FBZZ Engine
// AnimatorSystem.hpp | fbzz::scene
// スケルタルアニメーションのサンプリングと GPU 転送
// AnimationClip を評価し、SkinnedMeshRenderer 用の骨行列を更新する。
// 描画そのものは RenderSystem に任せる。
#pragma once

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::scene {

class Scene;

void AnimatorSystem(Scene& scene, renderer::ResourceManager& resources, float dt);

} // namespace fbzz::scene
