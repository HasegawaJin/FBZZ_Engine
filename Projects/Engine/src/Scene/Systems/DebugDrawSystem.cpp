// FBZZ Engine
// DebugDrawSystem.cpp | fbzz::scene
// デバッグワイヤー描画 System の実装
// Collider / アニメーター骨格 / 物理拘束 の可視化をまとめる。
#include <Engine/Scene/Systems/DebugDrawSystem.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Physics/ColliderDebugGeometry.hpp>
#include <Physics/ConvexHullCollider.hpp>
#include <Physics/TriangleMeshCollider.hpp>
#include <Physics/ConstraintDebugGeometry.hpp>
#include <Physics/World.hpp>
#include <Math/Vector3.hpp>
#include <type_traits>

namespace fbzz::scene
{

// ---------------------------------------------------------------------------
// ColliderDebugDrawSystem
// ---------------------------------------------------------------------------
namespace {

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
    for (auto& go : scene.GameObjects()) {
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

// ---------------------------------------------------------------------------
// AnimatorDebugDrawSystem
// ---------------------------------------------------------------------------
void AnimatorDebugDrawSystem(Scene& scene,
                              renderer::IRenderer& renderer,
                              renderer::ResourceManager& resources,
                              const math::Matrix4& viewProjection,
                              const math::Vector4& boneColor,
                              const math::Vector4& jointColor)
{
    renderer::DebugDraw::BeginFrame(renderer, resources, viewProjection);
    for (auto& go : scene.GameObjects()) {
        auto* anim = go.GetComponent<AnimatorComponent>();
        if (!anim || !anim->enabled) continue;
        if (anim->nodeGlobalTransforms.empty()) continue;
        auto* smr = go.GetComponent<SkinnedMeshRenderer>();
        if (!smr || !smr->model || !smr->model->skeleton) continue;
        const auto& skeleton = *smr->model->skeleton;
        const math::Matrix4 worldMatrix = go.transform.GetWorldMatrix();
        for (size_t ni = 0; ni < skeleton.nodes.size(); ++ni) {
            const auto& node = skeleton.nodes[ni];
            if (node.parentIndex < 0 ||
                node.parentIndex >= static_cast<int>(anim->nodeGlobalTransforms.size()))
                continue;
            const math::Matrix4& childG  = anim->nodeGlobalTransforms[ni];
            const math::Matrix4& parentG = anim->nodeGlobalTransforms[static_cast<size_t>(node.parentIndex)];
            auto ToWorld = [&worldMatrix](const math::Matrix4& g) -> math::Vector3 {
                math::Vector4 p = worldMatrix * math::Vector4{ g.m[0][3], g.m[1][3], g.m[2][3], 1.0f };
                return { p.x, p.y, p.z };
            };
            renderer::DebugDraw::Line(renderer, ToWorld(parentG), ToWorld(childG), boneColor);
            if (node.boneIndex >= 0) {
                math::Vector3 pos = ToWorld(childG);
                constexpr float r = 0.01f;
                renderer::DebugDraw::Box(renderer, pos, { r, r, r }, jointColor);
            }
        }
    }
    renderer::DebugDraw::Flush();
}

// ---------------------------------------------------------------------------
// ConstraintDebugDrawSystem
// ---------------------------------------------------------------------------
void ConstraintDebugDrawSystem(const physics::World& world,
                               renderer::IRenderer& renderer,
                               const math::Vector4& color)
{
    for (const auto& constraint : world.GetConstraints()) {
        if (!constraint) continue;
        const physics::ConstraintDebugGeometry geometry =
            physics::BuildConstraintDebugGeometry(*constraint);
        for (const physics::DebugLine& line : geometry.lines)
            renderer::DebugDraw::Line(renderer, line.from, line.to, color);
    }
}

} // namespace fbzz::scene
