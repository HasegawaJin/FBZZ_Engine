// FBZZ Engine
// DebugCollidersPass.cpp | fbzz::scene
// コライダーワイヤーフレームを HDR バッファへ描画する IRenderPass 実装
#include "DebugPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Systems/ColliderSync.hpp>
#include <Physics/ColliderDebugGeometry.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

std::string_view DebugCollidersPass::Name() const { return "DebugColliders"; }

std::vector<renderer::RenderGraph::ResourceAccess> DebugCollidersPass::DeclareAccesses(const RenderPassContext&) const
{
    using U = renderer::RenderGraph::ResourceUsage;
    return { { "HDR", U::ReadWrite } };
}

namespace {

// コライダー 1 個分のワイヤーを積む。
// WHY: PhysicsSystem は RunMode::SimOnly のため、エディタ停止中は Collider の遅延構築も
//      形状同期も走らない。可視化側でも PrepareCollider() を通すことで
//      「再生していないと Mesh / ConvexHull / Terrain コライダーが表示されない」
//      「Inspector で size を変えてもワイヤーが追従しない」の両方を解消する。
template<typename T>
void DrawCollider(Scene& scene, GameObject& go, T& collider,
                  renderer::IRenderer& renderer, const math::Vector4& color)
{
    if (!collider.enabled) return;
    if (!PrepareCollider(scene, go, collider)) return;

    const physics::ColliderDebugGeometry geometry =
        physics::BuildColliderDebugGeometry(*collider.collider);

    // WHY: DebugDraw のバッチは 1 回の Flush で 65536 頂点まで。メッシュコライダーは
    //      1 個で最大 8192 ライン (16384 頂点) を出すため、数個並ぶだけで上限に達し、
    //      以降のオブジェクトが黙って描画されなくなっていた。
    //      溢れる前に中間 Flush して、全オブジェクト分を確実に描く。
    if (renderer::DebugDraw::PendingLineVertices() + geometry.lines.size() * 2
        > renderer::DebugDraw::MaxBatchVertices()) {
        renderer::DebugDraw::Flush();
    }

    for (const physics::DebugLine& line : geometry.lines)
        renderer::DebugDraw::Line(renderer, line.from, line.to, color);
}

// 指定した Collider 型を持つ全エンティティを描く。
// WHY: GameObject を総なめして GetComponent するより、ComponentArray の Entity span を
//      入口にした方が PhysicsSystem と走査対象が完全に一致する (数え漏れが起きない)。
template<typename T>
void DrawCollidersOfType(RenderPassContext& ctx, const math::Vector4& color)
{
    for (EntityID id : ctx.scene.GetEntities<T>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        T* col = ctx.scene.GetComponent<T>(id);
        if (!go || !col) continue;
        DrawCollider(ctx.scene, *go, *col, ctx.renderer, color);
    }
}

} // namespace

void DebugCollidersPass::Execute(RenderPassContext& ctx)
{
    if (!ctx.settings.showColliders) return;

    constexpr math::Vector4 kColor = { 0.1f, 1.0f, 0.35f, 1.0f };
    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetViewProjection());

    DrawCollidersOfType<AabbColliderComponent>(ctx, kColor);
    DrawCollidersOfType<BoxColliderComponent>(ctx, kColor);
    DrawCollidersOfType<SphereColliderComponent>(ctx, kColor);
    DrawCollidersOfType<CapsuleColliderComponent>(ctx, kColor);
    DrawCollidersOfType<MeshColliderComponent>(ctx, kColor);
    DrawCollidersOfType<ConvexHullColliderComponent>(ctx, kColor);
    // TerrainCollider は従来この一覧から漏れており、地形の当たり判定だけ
    // showColliders で一切表示されなかった。
    DrawCollidersOfType<TerrainColliderComponent>(ctx, kColor);

    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
