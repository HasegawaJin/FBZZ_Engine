/// @file    LightRangeDebugPass.cpp
/// @brief   Point / Spot / 面光源の形と影響範囲をワイヤーで描く。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include "DebugPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::scene {

std::string_view LightRangeDebugPass::Name() const { return "LightRangeDebug"; }

bool LightRangeDebugPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.showLightRange;
}

void LightRangeDebugPass::Execute(PassResources&, RenderPassContext& ctx)
{
    constexpr math::Vector4 kColor   = { 1.0f, 0.90f, 0.30f, 1.0f };
    constexpr float         kDeg2Rad = 3.14159265f / 180.0f;

    /// @note 範囲は淡く、光源の形は濃く描く。Tube / Area は «どこから» と «どこまで» が別物。
    const math::Vector4 kShape = kColor;
    const math::Vector4 kRange = { kColor.x, kColor.y, kColor.z, kColor.w * 0.45f };

    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetGpuViewProjection());
    for (auto& go : ctx.scene.GameObjects()) {
        if (!go.activeInHierarchy()) continue;
        const auto* light = go.GetComponent<LightComponent>();
        if (!light || !light->enabled) continue;
        const math::Vector3 pos = go.transform.worldPosition;
        switch (light->type) {
        case LightComponent::Type::Point:
            renderer::DebugDraw::Sphere(ctx.renderer, pos, light->range, kRange);
            if (light->sourceRadius > 0.0f)
                renderer::DebugDraw::Sphere(ctx.renderer, pos, light->sourceRadius, kShape);
            break;

        case LightComponent::Type::Spot: {
            const math::Vector3 dir         = go.transform.forward;
            const float         outerRadius = std::tan(light->outerCone * kDeg2Rad) * light->range;
            const float         innerRadius = std::tan(light->innerCone * kDeg2Rad) * light->range;
            renderer::DebugDraw::Cone(ctx.renderer, pos, dir, light->range, outerRadius, kShape);
            renderer::DebugDraw::Cone(ctx.renderer, pos, dir, light->range, innerRadius, kRange);
            break;
        }

        case LightComponent::Type::Sphere:
            renderer::DebugDraw::Sphere(ctx.renderer, pos, light->range, kRange);
            renderer::DebugDraw::Sphere(ctx.renderer, pos,
                                        (std::max)(light->sourceRadius, 0.05f), kShape);
            break;

        case LightComponent::Type::Tube: {
            /// @note 管の軸は Right (ライト収集と同じ規約)。Capsule の軸はローカル Y なので Z まわり -90 度を挟む。
            const math::Quaternion toX =
                math::Quaternion::FromAxisAngle({ 0.0f, 0.0f, 1.0f }, -3.14159265f * 0.5f);
            const math::Quaternion rot = go.transform.worldRotation * toX;
            const float half = (std::max)(light->sourceLength, 0.0f) * 0.5f;
            const float r    = (std::max)(light->sourceRadius, 0.02f);
            renderer::DebugDraw::Capsule(ctx.renderer, pos, r, half, rot, kShape);
            renderer::DebugDraw::Capsule(ctx.renderer, pos, light->range, half, rot, kRange);
            break;
        }

        case LightComponent::Type::Area: {
            /// @note 面の法線は Forward、幅が Right、高さが Up。
            const math::Vector3 n  = go.transform.forward;
            const math::Vector3 rt = go.transform.right;
            const math::Vector3 up = go.transform.up;
            const float hw = (std::max)(light->areaWidth,  0.001f) * 0.5f;
            const float hh = (std::max)(light->areaHeight, 0.001f) * 0.5f;
            const math::Vector3 quad[4] = {
                pos - rt * hw - up * hh, pos + rt * hw - up * hh,
                pos + rt * hw + up * hh, pos - rt * hw + up * hh,
            };
            renderer::DebugDraw::Polyline(ctx.renderer, quad, true, kShape);
            const float arrow = (std::min)(light->range * 0.25f, 3.0f);
            renderer::DebugDraw::Arrow(ctx.renderer, pos, pos + n * arrow, 0.2f, 0.06f, kShape);
            if (light->areaTwoSided)
                renderer::DebugDraw::Arrow(ctx.renderer, pos, pos - n * arrow, 0.2f, 0.06f, kShape);
            renderer::DebugDraw::Sphere(ctx.renderer, pos, light->range, kRange);
            break;
        }

        case LightComponent::Type::Directional:
        default:
            break;
        }
    }
    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
