// FBZZ Engine
// RenderSystem.hpp | fbzz::scene
// MeshRenderer + Transform を走査し、DrawCall を発行する
#pragma once
#include <memory>
#include <Engine/Renderer/ResourceHandle.hpp>

namespace fbzz::scene   { class Scene; }
namespace fbzz::renderer {
    class IRenderer;
    class Camera;
    class ResourceManager;
    struct RenderSettings;
}

namespace fbzz::scene {

void RenderSystem(Scene& scene,
                  renderer::IRenderer& renderer,
                  renderer::ResourceManager& resources,
                  const renderer::Camera& camera,
                  renderer::ResourceHandle<renderer::RenderTargetTag> outputRT = {},
                  const renderer::RenderSettings* settings = nullptr);

} // namespace fbzz::scene
