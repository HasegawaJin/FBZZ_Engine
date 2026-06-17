// FBZZ Engine
// LightRangeDebugPass.cpp | fbzz::scene
// Point / Spot ライトの影響範囲を HDR バッファへワイヤーで描画する IRenderPass 実装
#include "DebugPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <cmath>

namespace fbzz::scene {

std::string_view LightRangeDebugPass::Name() const { return "LightRangeDebug"; }

std::vector<renderer::RenderGraph::ResourceAccess> LightRangeDebugPass::DeclareAccesses(const RenderPassContext&) const
{
    using U = renderer::RenderGraph::ResourceUsage;
    return { { "HDR", U::ReadWrite } };
}

void LightRangeDebugPass::Execute(RenderPassContext& ctx)
{
    if (!ctx.settings.showLightRange) return;

    constexpr math::Vector4 kColor   = { 1.0f, 0.90f, 0.30f, 1.0f };
    constexpr float         kDeg2Rad = 3.14159265f / 180.0f;

    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetViewProjection());
    for (auto& go : ctx.scene.GameObjects()) {
        const auto* light = go.GetComponent<LightComponent>();
        if (!light || !light->enabled) continue;
        const math::Vector3 pos = go.transform.worldPosition;
        if (light->type == LightComponent::Type::Point) {
            renderer::DebugDraw::Sphere(ctx.renderer, pos, light->range, kColor);
        } else if (light->type == LightComponent::Type::Spot) {
            const math::Vector3 dir         = go.transform.forward;
            const float         outerRadius = std::tan(light->outerCone * kDeg2Rad) * light->range;
            const float         innerRadius = std::tan(light->innerCone * kDeg2Rad) * light->range;
            renderer::DebugDraw::Cone(ctx.renderer, pos, dir, light->range, outerRadius, kColor);
            // 内側コーンを半透明気味の同色で追加表示
            const math::Vector4 innerColor = { kColor.x, kColor.y, kColor.z, kColor.w * 0.5f };
            renderer::DebugDraw::Cone(ctx.renderer, pos, dir, light->range, innerRadius, innerColor);
        }
        // Directional は範囲なし — スキップ
    }
    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
