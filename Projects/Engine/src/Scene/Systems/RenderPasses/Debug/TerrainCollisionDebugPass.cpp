/// @file    TerrainCollisionDebugPass.cpp
/// @brief   TerrainCollider の HeightField 形状を LOD ワイヤーで HDR バッファへ描画する IRenderPass 実装。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// 近景: 実コリジョン三角形のワイヤー / 遠景: BVH ノードの AABB ボックス
#include "DebugPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Systems/ColliderSync.hpp>
#include <Physics/ColliderDebugGeometry.hpp>
#include <cstddef>

namespace fbzz::scene {

namespace {

// 三角形ワイヤーを実寸で出す半径 [m]。
constexpr float kDetailRadius = 80.0f;

} // namespace

std::string_view TerrainCollisionDebugPass::Name() const { return "TerrainCollisionDebug"; }

void TerrainCollisionDebugPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    // 描き先の束縛はフレームワークが行う (SetAutoTarget)。
    builder.ReadWrite("HDR").SetAutoTarget("HDR");
}

bool TerrainCollisionDebugPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.showTerrainCollision;
}

// WHY TerrainComponent ではなく TerrainColliderComponent を入口にするか:
//     以前は TerrainComponent の heightData を直接ワイヤー化していた。これは «地形の見た目» で
//     あって «当たり判定» ではない。HeightFieldCollider は BVH をワールド座標で持ち、
//     しかも静的前提で transform 変化時に再構築しない。生データから描くと、回転・スケール・
//     移動のどれが入ってもワイヤーだけが正しい位置へ動き、実際にぶつかる場所とずれる。
//     さらに Collider の無い地形にまで «コリジョン» が出ていた。
void TerrainCollisionDebugPass::Execute(PassResources&, RenderPassContext& ctx)
{
    constexpr math::Vector4 kDetailColor = { 0.1f, 1.0f, 0.35f, 1.0f };
    constexpr math::Vector4 kCoarseColor = { 0.2f, 0.8f, 0.2f, 0.6f };

    physics::ColliderDebugView view;
    view.cameraPosition = ctx.camera.m_position;
    view.detailRadius   = kDetailRadius;
    view.enabled        = true;

    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetViewProjection());
    for (EntityID id : ctx.scene.GetEntities<TerrainColliderComponent>()) {
        GameObject* go  = ctx.scene.GetGameObject(id);
        auto*       col = ctx.scene.GetComponent<TerrainColliderComponent>(id);
        if (!go || !col || !col->enabled || !go->activeInHierarchy()) continue;
        // WHY: PhysicsSystem は RunMode::SimOnly のため、エディタ停止中は BVH が構築されない。
        //      可視化側でも同じ手順を通して「再生していないと何も出ない」を防ぐ。
        if (!PrepareCollider(ctx.scene, *go, *col)) continue;

        const physics::ColliderDebugGeometry geometry =
            physics::BuildColliderDebugGeometry(*col->collider, view);

        // WHY: バッチが満杯になると以降の Line() は黙って捨てられる。地形が複数あると
        //      1 個あたり最大 12288 ライン積むため、溢れる前に中間 Flush する。
        if (renderer::DebugDraw::PendingLineVertices() + geometry.lines.size() * 2
            > renderer::DebugDraw::MaxBatchVertices()) {
            renderer::DebugDraw::Flush();
        }

        for (std::size_t i = 0; i < geometry.lines.size(); ++i) {
            const physics::DebugLine& line = geometry.lines[i];
            renderer::DebugDraw::Line(ctx.renderer, line.from, line.to,
                                      i < geometry.detailLineCount ? kDetailColor : kCoarseColor);
        }
    }
    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
