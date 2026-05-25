// FBZZ Engine
// RenderSystem.hpp | fbzz::scene
// Scene から DrawCall を生成する描画 System
// MeshRenderer / Light / Camera / Transform を集約し、IRenderer へ送信する。
// Renderer の具体実装には依存せず、ResourceManager とインターフェースだけを使う。
#pragma once
#include <Physics/Layer.hpp>
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
                  const renderer::RenderSettings* settings = nullptr,
                  fbzz::LayerMask cullingMask = fbzz::Layer::Everything);

} // namespace fbzz::scene
