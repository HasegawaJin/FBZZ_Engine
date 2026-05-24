// FBZZ Engine
// ConstraintDebugDrawSystem.hpp | fbzz::scene
// Physics constraint debug wire drawing
#pragma once
#include <Math/Vector4.hpp>

namespace fbzz::physics { class World; }
namespace fbzz::renderer { class IRenderer; }

namespace fbzz::scene
{
    void ConstraintDebugDrawSystem(const physics::World& world,
                                   renderer::IRenderer& renderer,
                                   const math::Vector4& color = { 1.0f, 0.82f, 0.18f, 1.0f });

} // namespace fbzz::scene
