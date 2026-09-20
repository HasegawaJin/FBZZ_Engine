/// @file    TerrainCollisionDebugPass.cpp
/// @brief   TerrainCollider の HeightField 形状を描く。近景は三角形、遠景は BVH ノードの箱。
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
#include <cstddef>

namespace fbzz::scene {

namespace {

/// 三角形ワイヤーを実寸で出す半径 [m]。
constexpr float kDetailRadius = 80.0f;

} // namespace

std::string_view TerrainCollisionDebugPass::Name() const { return "TerrainCollisionDebug"; }

bool TerrainCollisionDebugPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.showTerrainCollision;
}

/// @note 入口は TerrainColliderComponent。見た目 (heightData) ではなく、BVH がワールドに持つ当たり判定を描く。
void TerrainCollisionDebugPass::Execute(PassResources&, RenderPassContext& ctx)
{
    constexpr math::Vector4 kDetailColor = { 0.1f, 1.0f, 0.35f, 1.0f };
    constexpr math::Vector4 kCoarseColor = { 0.2f, 0.8f, 0.2f, 0.6f };

    physics::ColliderDebugView view;
    view.cameraPosition = ctx.camera.m_position;
    view.detailRadius   = kDetailRadius;
    view.enabled        = true;

    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetGpuViewProjection());
    for (EntityID id : ctx.scene.GetEntities<TerrainColliderComponent>()) {
        GameObject* go  = ctx.scene.GetGameObject(id);
        auto*       col = ctx.scene.GetComponent<TerrainColliderComponent>(id);
        if (!go || !col || !col->enabled || !go->activeInHierarchy()) continue;
        /// @note 停止中は PhysicsSystem (SimOnly) が BVH を作らないので、ここで同じ手順を通す。
        if (!PrepareCollider(ctx.scene, *go, *col)) continue;

        const physics::ColliderDebugGeometry geometry =
            physics::BuildColliderDebugGeometry(*col->collider, view);
        for (std::size_t i = 0; i < geometry.lines.size(); ++i) {
            const physics::DebugLine& line = geometry.lines[i];
            renderer::DebugDraw::Line(ctx.renderer, line.from, line.to,
                                      i < geometry.detailLineCount ? kDetailColor : kCoarseColor);
        }
    }
    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
