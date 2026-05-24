// FBZZ Engine
// ColliderDebugDrawSystem.cpp | fbzz::scene
// ColliderComponent のデバッグワイヤー描画
// Scene の Collider と Transform を読み、DebugDraw へ形状を渡す。
// 物理計算には関与せず、可視化だけを担当する。
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
