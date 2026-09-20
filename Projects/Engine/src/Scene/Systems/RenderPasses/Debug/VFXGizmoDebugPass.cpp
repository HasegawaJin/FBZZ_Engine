/// @file    VFXGizmoDebugPass.cpp
/// @brief   エミッターの発生形状と初速をワイヤーで描く。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// @note 表示は操作用 Preview / Scene View 限定。AI capture の評価画像にギズモを写さない。
/// @see FlowFieldDebugPass.cpp (流れの場はこちらから分けた)
#include "DebugPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::scene {

namespace {

void DrawEmitterShapes(RenderPassContext& ctx)
{
    constexpr math::Vector4 kShapeColor    = { 0.40f, 0.95f, 0.80f, 1.0f };
    constexpr math::Vector4 kVelocityColor = { 1.00f, 0.72f, 0.30f, 1.0f };

    for (auto& object : ctx.scene.GameObjects()) {
        if (!object.activeInHierarchy()) continue;
        const auto* emitter = object.GetComponent<ParticleEmitter>();
        if (emitter == nullptr || !emitter->settings.enabled) continue;
        const math::Vector3 origin = object.transform.worldPosition
            + object.transform.worldRotation * emitter->settings.emitPosition;

        switch (emitter->settings.shape) {
        case ParticleEmitterShape::Sphere:
            renderer::DebugDraw::Sphere(ctx.renderer, origin,
                                        (std::max)(emitter->settings.sphereRadius, 0.001f), kShapeColor);
            break;
        case ParticleEmitterShape::Cone: {
            /// @note SpawnParticle は +Y をコーン軸に取る。
            const math::Vector3 axis = object.transform.worldRotation * math::Vector3{ 0.0f, 1.0f, 0.0f };
            constexpr float kDeg2Rad = 3.14159265f / 180.0f;
            const float height = (std::max)(emitter->settings.coneRadius, 0.5f);
            const float baseRadius = emitter->settings.coneRadius
                + std::tan((std::max)(emitter->settings.coneAngleDegrees, 0.0f) * kDeg2Rad) * height;
            renderer::DebugDraw::Cone(ctx.renderer, origin, axis, height, baseRadius, kShapeColor);
            break;
        }
        case ParticleEmitterShape::Box:
            renderer::DebugDraw::Box(ctx.renderer, origin, emitter->settings.boxExtents,
                                     object.transform.worldRotation, kShapeColor);
            break;
        case ParticleEmitterShape::Point:
        case ParticleEmitterShape::MeshSurface:
        default:
            renderer::DebugDraw::Sphere(ctx.renderer, origin, 0.06f, kShapeColor);
            break;
        }

        /// @note 初速は 1 秒後の到達点を先端にする (長さがそのまま秒速)。
        const math::Vector3 velocity = object.transform.worldRotation * emitter->settings.emitVelocity;
        const float speed = velocity.Length();
        if (speed <= 1.0e-4f) continue;
        renderer::DebugDraw::Arrow(ctx.renderer, origin, origin + velocity,
                                   (std::min)(speed * 0.18f, 0.4f),
                                   (std::min)(speed * 0.06f, 0.12f), kVelocityColor);
    }
}

} // namespace

std::string_view VFXGizmoDebugPass::Name() const { return "VFXGizmoDebug"; }

bool VFXGizmoDebugPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.showVFXGizmos;
}

void VFXGizmoDebugPass::Execute(PassResources&, RenderPassContext& ctx)
{
    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetGpuViewProjection());
    DrawEmitterShapes(ctx);
    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
