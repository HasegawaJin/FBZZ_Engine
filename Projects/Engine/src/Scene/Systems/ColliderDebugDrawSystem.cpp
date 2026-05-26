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
#include <Physics/ConvexHullCollider.hpp>
#include <Physics/TriangleMeshCollider.hpp>
#include <type_traits>

namespace fbzz::scene
{
    namespace
    {
        math::Vector3 ComponentScale(const math::Vector3& a, const math::Vector3& b)
        {
            return { a.x * b.x, a.y * b.y, a.z * b.z };
        }

        math::Vector3 ColliderWorldCenter(const GameObject& go, const ColliderComponent& collider)
        {
            return go.transform.position +
                   go.transform.rotation * ComponentScale(collider.center, go.transform.worldScale);
        }

        template<typename T>
        void DrawCollider(T& collider,
                          GameObject& go,
                          renderer::IRenderer& renderer,
                          const math::Vector4& color)
        {
            if (!collider.enabled || !collider.collider) return;

            const math::Vector3 worldCenter = ColliderWorldCenter(go, collider);
            if (auto* mesh = collider.collider->GetType() == physics::ColliderType::TRIANGLE_MESH
                    ? static_cast<physics::TriangleMeshCollider*>(collider.collider.get())
                    : nullptr) {
                math::Vector3 scale = go.transform.worldScale;
                if constexpr (std::is_same_v<T, MeshColliderComponent>) {
                    if (!collider.useTransformScale)
                        scale = math::Vector3::ONE;
                }
                mesh->UpdateWithScale(worldCenter, go.transform.rotation, scale);
            } else if (auto* hull = collider.collider->GetType() == physics::ColliderType::CONVEX_HULL
                    ? static_cast<physics::ConvexHullCollider*>(collider.collider.get())
                    : nullptr) {
                math::Vector3 scale = go.transform.worldScale;
                if constexpr (std::is_same_v<T, ConvexHullColliderComponent>) {
                    if (!collider.useTransformScale)
                        scale = math::Vector3::ONE;
                }
                hull->UpdateWithScale(worldCenter, go.transform.rotation, scale);
            } else {
                collider.collider->Update(worldCenter, go.transform.rotation);
            }
            const physics::ColliderDebugGeometry geometry =
                physics::BuildColliderDebugGeometry(*collider.collider);

            for (const physics::DebugLine& line : geometry.lines)
                renderer::DebugDraw::Line(renderer, line.from, line.to, color);
        }
    } // namespace

    void ColliderDebugDrawSystem(Scene& scene,
                                 renderer::IRenderer& renderer,
                                 const math::Vector4& color)
    {
        for (auto& go : scene.GameObjects())
        {
            if (auto* collider = go.GetComponent<AabbColliderComponent>())
                DrawCollider(*collider, go, renderer, color);
            if (auto* collider = go.GetComponent<BoxColliderComponent>())
                DrawCollider(*collider, go, renderer, color);
            if (auto* collider = go.GetComponent<SphereColliderComponent>())
                DrawCollider(*collider, go, renderer, color);
            if (auto* collider = go.GetComponent<CapsuleColliderComponent>())
                DrawCollider(*collider, go, renderer, color);
            if (auto* collider = go.GetComponent<MeshColliderComponent>())
                DrawCollider(*collider, go, renderer, color);
            if (auto* collider = go.GetComponent<ConvexHullColliderComponent>())
                DrawCollider(*collider, go, renderer, color);
        }
    }

} // namespace fbzz::scene
