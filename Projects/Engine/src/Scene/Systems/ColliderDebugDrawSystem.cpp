// FBZZ Engine
// ColliderDebugDrawSystem.cpp | fbzz::scene
// Scene collider debug wire drawing
#include <Engine/Scene/Systems/ColliderDebugDrawSystem.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Physics/ColliderDebugGeometry.hpp>

namespace fbzz::scene
{
    void ColliderDebugDrawSystem(Scene& scene,
                                 renderer::IRenderer& renderer,
                                 const math::Vector4& color)
    {
        for (auto& go : scene.GameObjects())
        {
            auto* collider = go.GetComponent<ColliderComponent>();
            if (!collider || !collider->enabled || !collider->collider) continue;

            collider->collider->Update(go.transform.position, go.transform.rotation);
            const physics::ColliderDebugGeometry geometry =
                physics::BuildColliderDebugGeometry(*collider->collider);

            for (const physics::DebugLine& line : geometry.lines)
                renderer::DebugDraw::Line(renderer, line.from, line.to, color);
        }
    }

} // namespace fbzz::scene
