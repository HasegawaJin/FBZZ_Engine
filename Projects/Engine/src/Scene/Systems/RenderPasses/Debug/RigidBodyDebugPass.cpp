/// @file    RigidBodyDebugPass.cpp
/// @brief   選択中 RigidBody の速度矢印 (1 秒後の到達点) と重心を描く。
/// @author  Hasegawa Jin
/// @date    2026-09-16
#include "DebugPasses.hpp"
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Physics/RigidBody.hpp>
#include <algorithm>

namespace fbzz::scene {

std::string_view RigidBodyDebugPass::Name() const { return "RigidBodyDebug"; }

/// @note 全剛体に矢印を出すと群衆や瓦礫で画面が埋まるので、選択中だけに絞る。
bool RigidBodyDebugPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.showRigidBodies && !ctx.settings.selectedObjects.empty();
}

void RigidBodyDebugPass::Execute(PassResources&, RenderPassContext& ctx)
{
    constexpr math::Vector4 kVelocityColor = { 1.00f, 0.45f, 0.20f, 1.0f };
    constexpr math::Vector4 kAngularColor  = { 0.55f, 0.55f, 1.00f, 1.0f };
    constexpr math::Vector4 kCenterColor   = { 1.00f, 1.00f, 1.00f, 1.0f };
    constexpr math::Vector4 kSleepColor    = { 0.50f, 0.50f, 0.55f, 1.0f };

    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetViewProjection());
    for (EntityID id : ctx.scene.GetEntities<RigidBodyComponent>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        auto* rb = ctx.scene.GetComponent<RigidBodyComponent>(id);
        if (!go || !rb || !rb->enabled || !go->activeInHierarchy()) continue;
        if (!IsSelectedForDebug(*go, ctx)) continue;

        /// @note 剛体の実体は Play 中しか無い。編集中は重心の代わりに Transform の位置だけ示す。
        const physics::RigidBody* body = rb->rigidBody.get();
        const math::Vector3 center = body ? body->GetPosition() : go->transform.worldPosition;
        const float distance = (center - ctx.camera.m_position).Length();
        const float marker = std::clamp(distance * 0.01f, 0.03f, 0.5f);
        const math::Vector4 centerColor = (body && body->IsSleeping()) ? kSleepColor : kCenterColor;
        renderer::DebugDraw::Line(ctx.renderer, center - math::Vector3{ marker, 0, 0 },
                                  center + math::Vector3{ marker, 0, 0 }, centerColor);
        renderer::DebugDraw::Line(ctx.renderer, center - math::Vector3{ 0, marker, 0 },
                                  center + math::Vector3{ 0, marker, 0 }, centerColor);
        renderer::DebugDraw::Line(ctx.renderer, center - math::Vector3{ 0, 0, marker },
                                  center + math::Vector3{ 0, 0, marker }, centerColor);
        if (!body || body->IsStatic()) continue;

        const math::Vector3 velocity = body->GetVelocity();
        const float speed = velocity.Length();
        if (speed > 1e-3f)
            renderer::DebugDraw::Arrow(ctx.renderer, center, center + velocity,
                                       (std::min)(speed * 0.2f, 0.4f), (std::min)(speed * 0.06f, 0.12f),
                                       kVelocityColor);

        /// @note 角速度は回転軸の向き、長さは rad/s。
        const math::Vector3 angular = body->GetAngularVelocity();
        const float spin = angular.Length();
        if (spin > 1e-3f)
            renderer::DebugDraw::Arrow(ctx.renderer, center, center + angular * 0.25f,
                                       (std::min)(spin * 0.05f, 0.2f), (std::min)(spin * 0.015f, 0.06f),
                                       kAngularColor);
    }
    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
