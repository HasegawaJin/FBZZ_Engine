// FBZZ Engine
// ConstraintDebugDrawSystem.cpp | fbzz::scene
// Physics constraint debug wire drawing
#include <Engine/Scene/Systems/ConstraintDebugDrawSystem.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Physics/ConstraintDebugGeometry.hpp>
#include <Physics/World.hpp>

namespace fbzz::scene
{
    void ConstraintDebugDrawSystem(const physics::World& world,
                                   renderer::IRenderer& renderer,
                                   const math::Vector4& color)
    {
        for (const auto& constraint : world.GetConstraints())
        {
            if (!constraint) continue;
            const physics::ConstraintDebugGeometry geometry =
                physics::BuildConstraintDebugGeometry(*constraint);

            for (const physics::DebugLine& line : geometry.lines)
                renderer::DebugDraw::Line(renderer, line.from, line.to, color);
        }
    }

} // namespace fbzz::scene
