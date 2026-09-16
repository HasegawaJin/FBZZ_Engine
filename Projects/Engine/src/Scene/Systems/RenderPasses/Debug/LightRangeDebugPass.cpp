/// @file    LightRangeDebugPass.cpp
/// @brief   Point / Spot ライトの影響範囲を HDR バッファへワイヤーで描画する IRenderPass 実装。
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

void LightRangeDebugPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    // 描き先の束縛はフレームワークが行う (SetAutoTarget)。
    builder.ReadWrite("HDR").SetAutoTarget("HDR");
}

bool LightRangeDebugPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.showLightRange;
}

void LightRangeDebugPass::Execute(PassResources&, RenderPassContext& ctx)
{
    constexpr math::Vector4 kColor   = { 1.0f, 0.90f, 0.30f, 1.0f };
    constexpr float         kDeg2Rad = 3.14159265f / 180.0f;

    // 範囲は淡く、光源そのものの形 (球の半径 / 管の長さ / 面の大きさ) は濃く描く。
    // WHY 分けるか: Tube や Area は「どこから出ているか」と「どこまで届くか」が
    //     別物で、同じ濃さで描くと線が団子になってどちらも読めない。
    const math::Vector4 kShape = kColor;
    const math::Vector4 kRange = { kColor.x, kColor.y, kColor.z, kColor.w * 0.45f };

    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetViewProjection());
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
            // 管の軸は Transform の Right (RenderSystem のライト収集と同じ規約)。
            // 一方 DebugDraw::Capsule の軸は回転後の Y なので、Y を X へ送る
            // -90 度 (Z 軸まわり) を挟む。これを忘れるとカプセルが管と直交して出る。
            const math::Quaternion toX =
                math::Quaternion::FromAxisAngle({ 0.0f, 0.0f, 1.0f }, -3.14159265f * 0.5f);
            const math::Quaternion rot = go.transform.worldRotation * toX;
            const float half = (std::max)(light->sourceLength, 0.0f) * 0.5f;
            const float r    = (std::max)(light->sourceRadius, 0.02f);
            renderer::DebugDraw::Capsule(ctx.renderer, pos, r, half, rot, kShape);
            // 届く範囲は「管を range ぶん太らせた形」なので、同じ姿勢のカプセルで表す。
            renderer::DebugDraw::Capsule(ctx.renderer, pos, light->range, half, rot, kRange);
            break;
        }

        case LightComponent::Type::Area: {
            // 面の法線は Forward、幅が Right、高さが Up。
            const math::Vector3 n  = go.transform.forward;
            const math::Vector3 rt = go.transform.right;
            const math::Vector3 up = go.transform.up;
            const float hw = (std::max)(light->areaWidth,  0.001f) * 0.5f;
            const float hh = (std::max)(light->areaHeight, 0.001f) * 0.5f;
            const math::Vector3 quad[4] = {
                pos - rt * hw - up * hh, pos + rt * hw - up * hh,
                pos + rt * hw + up * hh, pos - rt * hw + up * hh,
            };
            for (int i = 0; i < 4; ++i)
                renderer::DebugDraw::Line(ctx.renderer, quad[i], quad[(i + 1) % 4], kShape);
            // どちらへ照らす面かを矢印で示す。両面なら裏へも出す。
            const float arrow = (std::min)(light->range * 0.25f, 3.0f);
            renderer::DebugDraw::Arrow(ctx.renderer, pos, pos + n * arrow, 0.2f, 0.06f, kShape);
            if (light->areaTwoSided)
                renderer::DebugDraw::Arrow(ctx.renderer, pos, pos - n * arrow, 0.2f, 0.06f, kShape);
            renderer::DebugDraw::Sphere(ctx.renderer, pos, light->range, kRange);
            break;
        }

        case LightComponent::Type::Directional:
        default:
            break;  // 範囲を持たない
        }
    }
    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
