// FBZZ Engine
// ColliderDebugDrawSystem.hpp | fbzz::scene
// Scene collider debug wire drawing
#pragma once
#include <Math/Vector4.hpp>

namespace fbzz::renderer { class IRenderer; }
namespace fbzz::scene { class Scene; }

namespace fbzz::scene
{
    void ColliderDebugDrawSystem(Scene& scene,
                                 renderer::IRenderer& renderer,
                                 const math::Vector4& color = { 0.1f, 1.0f, 0.35f, 1.0f });

} // namespace fbzz::scene
