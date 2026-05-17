// FBZZ Engine
// RenderSystem.hpp | fbzz::scene
// MeshRenderer + Transform を走査し、DrawCall を発行する
#pragma once

namespace fbzz::scene   { class Scene; }
namespace fbzz::renderer {
    class IRenderer;
    class Camera;
    class LightSystem;
}

namespace fbzz::scene {

void RenderSystem(Scene& scene,
                  renderer::IRenderer& renderer,
                  const renderer::Camera& camera,
                  const renderer::LightSystem& lights);

} // namespace fbzz::scene
