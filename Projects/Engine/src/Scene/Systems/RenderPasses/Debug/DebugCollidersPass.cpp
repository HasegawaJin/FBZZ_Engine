// FBZZ Engine
// DebugCollidersPass.cpp | fbzz::scene
// コライダーワイヤーフレームを HDR バッファへ描画する IRenderPass 実装
#include "DebugPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Physics/ColliderDebugGeometry.hpp>
#include <Physics/ConvexHullCollider.hpp>
#include <Physics/HeightFieldCollider.hpp>
#include <Physics/TriangleMeshCollider.hpp>
#include <Math/Vector3.hpp>
#include <type_traits>

namespace fbzz::scene {

std::string_view DebugCollidersPass::Name() const { return "DebugColliders"; }

std::vector<renderer::RenderGraph::ResourceAccess> DebugCollidersPass::DeclareAccesses(const RenderPassContext&) const
{
    using U = renderer::RenderGraph::ResourceUsage;
    return { { "HDR", U::ReadWrite } };
}

namespace {

math::Vector3 ComponentScale(const math::Vector3& a, const math::Vector3& b)
{
    return { a.x * b.x, a.y * b.y, a.z * b.z };
}

math::Vector3 ColliderWorldCenter(const GameObject& go, const ColliderComponent& collider)
{
    return go.transform.worldPosition +
           go.transform.worldRotation * ComponentScale(collider.center, go.transform.worldScale);
}

template<typename T>
void DrawCollider(T& collider, GameObject& go, renderer::IRenderer& renderer, const math::Vector4& color)
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
        mesh->UpdateWithScale(worldCenter, go.transform.worldRotation, scale);
    } else if (auto* hf = collider.collider->GetType() == physics::ColliderType::HEIGHT_FIELD
            ? static_cast<physics::HeightFieldCollider*>(collider.collider.get())
            : nullptr) {
        hf->UpdateWithScale(worldCenter, go.transform.worldRotation, go.transform.worldScale);
    } else if (auto* hull = collider.collider->GetType() == physics::ColliderType::CONVEX_HULL
            ? static_cast<physics::ConvexHullCollider*>(collider.collider.get())
            : nullptr) {
        math::Vector3 scale = go.transform.worldScale;
        if constexpr (std::is_same_v<T, ConvexHullColliderComponent>) {
            if (!collider.useTransformScale)
                scale = math::Vector3::ONE;
        }
        hull->UpdateWithScale(worldCenter, go.transform.worldRotation, scale);
    } else {
        collider.collider->Update(worldCenter, go.transform.worldRotation);
    }
    const physics::ColliderDebugGeometry geometry =
        physics::BuildColliderDebugGeometry(*collider.collider);
    for (const physics::DebugLine& line : geometry.lines)
        renderer::DebugDraw::Line(renderer, line.from, line.to, color);
}

} // namespace

void DebugCollidersPass::Execute(RenderPassContext& ctx)
{
    if (!ctx.settings.showColliders) return;

    constexpr math::Vector4 kColor = { 0.1f, 1.0f, 0.35f, 1.0f };
    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetViewProjection());
    for (auto& go : ctx.scene.GameObjects()) {
        if (auto* c = go.GetComponent<AabbColliderComponent>())       DrawCollider(*c, go, ctx.renderer, kColor);
        if (auto* c = go.GetComponent<BoxColliderComponent>())        DrawCollider(*c, go, ctx.renderer, kColor);
        if (auto* c = go.GetComponent<SphereColliderComponent>())     DrawCollider(*c, go, ctx.renderer, kColor);
        if (auto* c = go.GetComponent<CapsuleColliderComponent>())    DrawCollider(*c, go, ctx.renderer, kColor);
        if (auto* c = go.GetComponent<MeshColliderComponent>())       DrawCollider(*c, go, ctx.renderer, kColor);
        if (auto* c = go.GetComponent<ConvexHullColliderComponent>()) DrawCollider(*c, go, ctx.renderer, kColor);
    }
    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
