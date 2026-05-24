// FBZZ Engine
// AnimatorSystem.hpp | fbzz::scene
// Skeletal animation sampling and skinning constant-buffer upload
#pragma once

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::scene {

class Scene;

void AnimatorSystem(Scene& scene, renderer::ResourceManager& resources, float dt);

} // namespace fbzz::scene
