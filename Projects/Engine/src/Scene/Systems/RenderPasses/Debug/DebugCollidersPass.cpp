/// @file    DebugCollidersPass.cpp
/// @brief   コライダーワイヤーフレームを HDR バッファへ描画する IRenderPass 実装。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include "DebugPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Systems/ColliderSync.hpp>
#include <Physics/ColliderDebugGeometry.hpp>
#include <Math/Vector3.hpp>
#include <cstddef>

namespace fbzz::scene {

std::string_view DebugCollidersPass::Name() const { return "DebugColliders"; }

void DebugCollidersPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    // 描き先の束縛はフレームワークが行う (SetAutoTarget)。
    builder.ReadWrite("HDR").SetAutoTarget("HDR");
}

namespace {

// 破線 1 本ぶんのワールド長と、1 線分あたりの分割上限。
constexpr float DASH_LENGTH = 0.06f;
constexpr int   DASH_MAX    = 8;

// これを超える本数を持つジオメトリは実線のまま描く。
// WHY: Mesh / Terrain コライダーは 1 個で数千本あり、破線にすると頂点数が桁で増える。
//      一方でスクリプトの Gizmo がメッシュコライダーの全辺と重なることはないので、
//      ちらつきを解く必要があるのはプリミティブ形状の側だけ。
constexpr std::size_t DASH_LINE_LIMIT = 512;

// 三角形ワイヤーを実寸で出す半径 [m]。外側は BVH ノードの箱になる。
constexpr float DETAIL_RADIUS = 80.0f;

math::Vector4 CoarseColor(const math::Vector4& color)
{
    return { color.x * 0.7f, color.y * 0.7f, color.z * 0.7f, color.w * 0.6f };
}

// コライダー 1 個分のワイヤーを積む。
// WHY: PhysicsSystem は RunMode::SimOnly のため、エディタ停止中は Collider の遅延構築も
//      形状同期も走らない。可視化側でも PrepareCollider() を通すことで
//      「再生していないと Mesh / ConvexHull / Terrain コライダーが表示されない」
//      「Inspector で size を変えてもワイヤーが追従しない」の両方を解消する。
template<typename T>
void DrawCollider(Scene& scene, GameObject& go, T& collider,
                  renderer::IRenderer& renderer, const math::Vector4& color,
                  const physics::ColliderDebugView& view)
{
    if (!collider.enabled) return;
    if (!PrepareCollider(scene, go, collider)) return;

    const physics::ColliderDebugGeometry geometry =
        physics::BuildColliderDebugGeometry(*collider.collider, view);

    // プリミティブ形状は破線で描く。Script の Gizmo が同じ位置に実線を出していても、
    // 隙間から下の線が見えるため両方読める (このパスは Gizmo より後に走る)。
    const bool dashed = geometry.lines.size() <= DASH_LINE_LIMIT;
    const std::size_t verticesPerLine = dashed
        ? renderer::DebugDraw::DashedLineMaxVertices(DASH_MAX) : 2u;

    // WHY: DebugDraw のバッチは 1 回の Flush で 65536 頂点まで。メッシュコライダーは
    //      1 個で最大 8192 ライン (16384 頂点) を出すため、数個並ぶだけで上限に達し、
    //      以降のオブジェクトが黙って描画されなくなっていた。
    //      溢れる前に中間 Flush して、全オブジェクト分を確実に描く。
    if (renderer::DebugDraw::PendingLineVertices() + geometry.lines.size() * verticesPerLine
        > renderer::DebugDraw::MaxBatchVertices()) {
        renderer::DebugDraw::Flush();
    }

    for (std::size_t i = 0; i < geometry.lines.size(); ++i) {
        const physics::DebugLine& line = geometry.lines[i];
        // detailLineCount より後ろは BVH ノードの箱。輪郭として読ませたいだけなので落とす。
        const math::Vector4 lineColor = i < geometry.detailLineCount ? color : CoarseColor(color);
        if (dashed)
            renderer::DebugDraw::LineDashed(renderer, line.from, line.to, lineColor,
                                            DASH_LENGTH, DASH_MAX);
        else
            renderer::DebugDraw::Line(renderer, line.from, line.to, lineColor);
    }
}

// 指定した Collider 型を持つ全エンティティを描く。
// WHY: GameObject を総なめして GetComponent するより、ComponentArray の Entity span を
//      入口にした方が PhysicsSystem と走査対象が完全に一致する (数え漏れが起きない)。
template<typename T>
void DrawCollidersOfType(RenderPassContext& ctx, const math::Vector4& color,
                         const physics::ColliderDebugView& view)
{
    for (EntityID id : ctx.scene.GetEntities<T>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        T* col = ctx.scene.GetComponent<T>(id);
        // WHY activeInHierarchy まで見るか: PhysicsSystem は非アクティブな GO の
        //     Collider を同期対象から外す。可視化だけ描いてしまうと、当たり判定が
        //     生きていない場所にワイヤーが残り「消したはずの壁に当たる」と誤読される。
        if (!go || !col || !go->activeInHierarchy()) continue;
        DrawCollider(ctx.scene, *go, *col, ctx.renderer, color, view);
    }
}

} // namespace

bool DebugCollidersPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.showColliders;
}

void DebugCollidersPass::Execute(PassResources&, RenderPassContext& ctx)
{
    constexpr math::Vector4 kColor = { 0.1f, 1.0f, 0.35f, 1.0f };

    // BVH コライダー (Mesh / Terrain) だけがこの視点を見る。プリミティブは影響を受けない。
    physics::ColliderDebugView view;
    view.cameraPosition = ctx.camera.m_position;
    view.detailRadius   = DETAIL_RADIUS;
    view.enabled        = true;

    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetViewProjection());

    DrawCollidersOfType<AabbColliderComponent>(ctx, kColor, view);
    DrawCollidersOfType<BoxColliderComponent>(ctx, kColor, view);
    DrawCollidersOfType<SphereColliderComponent>(ctx, kColor, view);
    DrawCollidersOfType<CapsuleColliderComponent>(ctx, kColor, view);
    DrawCollidersOfType<CylinderColliderComponent>(ctx, kColor, view);
    DrawCollidersOfType<MeshColliderComponent>(ctx, kColor, view);
    DrawCollidersOfType<ConvexHullColliderComponent>(ctx, kColor, view);
    // TerrainCollider は従来この一覧から漏れており、地形の当たり判定だけ
    // showColliders で一切表示されなかった。
    // showTerrainCollision が入っているときは TerrainCollisionDebugPass が同じ線を
    // 同じ色で描くので、ここでは譲る (二重に積むとバッチ頂点だけ倍食う)。
    if (!ctx.settings.showTerrainCollision)
        DrawCollidersOfType<TerrainColliderComponent>(ctx, kColor, view);

    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
