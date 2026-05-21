// FBZZ Engine
// RenderSystem.hpp | fbzz::scene
// MeshRenderer + Transform を走査し、DrawCall を発行する
#pragma once
#include <memory>

namespace fbzz::scene   { class Scene; }
namespace fbzz::renderer {
    class IRenderer;
    class IRenderTarget;
    class Camera;
    struct RenderSettings;
}

namespace fbzz::scene {

void RenderSystem(Scene& scene,
                  renderer::IRenderer& renderer,
                  const renderer::Camera& camera,
                  const std::shared_ptr<renderer::IRenderTarget>& outputRT = nullptr,
                  const renderer::RenderSettings* settings = nullptr);

} // namespace fbzz::scene
