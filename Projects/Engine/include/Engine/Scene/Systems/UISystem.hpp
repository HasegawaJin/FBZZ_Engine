// FBZZ Engine
// UISystem.hpp | fbzz::scene
// Runtime UI draw and button hit processing
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

// viewProjection: camera VP matrix for WorldSpace canvases (pass identity if no WorldSpace canvases).
void UISystem(Scene& scene,
              renderer::IRenderer& renderer,
              renderer::ResourceManager& resources,
              float viewportWidth,
              float viewportHeight,
              math::Vector2 mouseInCanvasSpace,
              bool mousePressed,
              const math::Matrix4& viewProjection = math::Matrix4::Identity());

} // namespace fbzz::scene
