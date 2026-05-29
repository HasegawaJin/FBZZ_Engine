// FBZZ Engine
// IKSystem.hpp | fbzz::scene
// AnimatorSystem が確定した FK ポーズに解析的 2-Bone IK を後処理として適用し、スキニング行列を再アップロードする。
#pragma once

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::scene {

class Scene;

void IKSystem(Scene& scene, renderer::ResourceManager& resources, float dt);

} // namespace fbzz::scene
