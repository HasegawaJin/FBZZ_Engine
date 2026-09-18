/// @file    FlowFieldDebugPass.cpp
/// @brief   FlowField の効く範囲・減衰の目安・流れの向きをワイヤーで描く。
/// @author  Hasegawa Jin
/// @date    2026-09-17
///
/// @note 範囲は ResolveFlowField の結果から描く。設定値を直に読むと Baked の radius
///       (解決時に 0 へ落ちる) のような «見えるのに効かない値» を描いてしまう。
/// @see Docs/design/flow-field.md
#include "DebugPasses.hpp"
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/Fields/FlowField.hpp>
#include <Engine/Scene/Fields/FlowFieldEval.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Fluid/VectorFieldAsset.hpp>
#include <algorithm>
#include <array>
#include <cmath>

namespace fbzz::scene {

namespace {

/// @brief 流れの種別ごとの色。同じ場所に重ねた場の半径を取り違えないため。
math::Vector4 FlowFieldColor(FlowFieldType type)
{
    switch (type) {
    case FlowFieldType::Uniform:    return { 0.35f, 0.85f, 1.00f, 1.0f };
    case FlowFieldType::Sink:       return { 0.45f, 1.00f, 0.55f, 1.0f };
    case FlowFieldType::Source:     return { 1.00f, 0.55f, 0.35f, 1.0f };
    case FlowFieldType::Vortex:     return { 0.80f, 0.55f, 1.00f, 1.0f };
    case FlowFieldType::Curl:       return { 1.00f, 0.85f, 0.35f, 1.0f };
    case FlowFieldType::LegacyDrag: return { 0.65f, 0.68f, 0.75f, 1.0f };
    case FlowFieldType::Baked:      return { 0.35f, 1.00f, 0.90f, 1.0f };
    }
    return { 1.0f, 1.0f, 1.0f, 1.0f };
}

constexpr math::Vector4 kMissingFieldColor = { 1.00f, 0.25f, 0.25f, 1.0f };

/// @note 非選択を消さず薄くする。重なった場のどれが効いているかは周りと見比べて読むため。
constexpr float kUnselectedAlpha = 0.45f;
/// @note radius <= 0 (全体に効く) の場は境界が無いので、中心の目印と向きだけこの大きさで描く。
constexpr float kGlobalGlyphRadius = 1.0f;
/// @note 減衰の目安に描く影響度。(1 - d/r)^p = 0.5 となる d の球。
constexpr float kFalloffGuideInfluence = 0.5f;

math::Vector4 WithAlpha(const math::Vector4& color, float alpha)
{
    return { color.x, color.y, color.z, color.w * alpha };
}

void DrawWireBox(renderer::IRenderer& r, const math::Vector3& center, const math::Vector3& halfExtents,
                 const math::Quaternion& rotation, const math::Vector4& color, bool dashed)
{
    if (!dashed) {
        renderer::DebugDraw::Box(r, center, halfExtents, rotation, color);
        return;
    }
    std::array<math::Vector3, 8> corners;
    for (int i = 0; i < 8; ++i) {
        const math::Vector3 local = {
            (i & 1) ? halfExtents.x : -halfExtents.x,
            (i & 2) ? halfExtents.y : -halfExtents.y,
            (i & 4) ? halfExtents.z : -halfExtents.z };
        corners[static_cast<size_t>(i)] = center + rotation * local;
    }
    /// @note 添字のビットが 1 つだけ違う角同士が辺。
    for (int i = 0; i < 8; ++i)
        for (int bit = 1; bit < 8; bit <<= 1)
            if ((i & bit) == 0)
                renderer::DebugDraw::LineDashed(r, corners[static_cast<size_t>(i)],
                                                corners[static_cast<size_t>(i | bit)], color, 0.15f, 16);
}

/// @brief 中心から外 (Source) または中 (Sink) へ向かう 6 本の矢印。
void DrawRadialArrows(renderer::IRenderer& r, const math::Vector3& center, float radius, bool outward,
                      const math::Vector4& color)
{
    static constexpr std::array<math::Vector3, 6> kDirections = {
        math::Vector3{ 1, 0, 0 }, math::Vector3{ -1, 0, 0 }, math::Vector3{ 0, 1, 0 },
        math::Vector3{ 0, -1, 0 }, math::Vector3{ 0, 0, 1 }, math::Vector3{ 0, 0, -1 } };
    const float inner = radius * 0.3f;
    const float outer = radius * 0.75f;
    const float head  = (outer - inner) * 0.3f;
    for (const math::Vector3& dir : kDirections) {
        const math::Vector3 a = center + dir * inner;
        const math::Vector3 b = center + dir * outer;
        renderer::DebugDraw::Arrow(r, outward ? a : b, outward ? b : a, head, head * 0.35f, color);
    }
}

void DrawField(renderer::IRenderer& r, const ActiveFlowField& field, const math::Quaternion& rotation,
               const math::Vector4& baseColor, bool dashed)
{
    const math::Vector3& center = field.position;

    if (field.type == FlowFieldType::Baked) {
        /// @note 未割り当て・読めない 速度場 PNG は «箱はあるのに何も起きない» ので、赤い対角線で知らせる。
        const bool missing = field.vectorField == nullptr || field.vectorField->Empty();
        DrawWireBox(r, center, field.extents, rotation, baseColor, dashed);
        if (missing) {
            const math::Vector4 warn = WithAlpha(kMissingFieldColor, baseColor.w);
            renderer::DebugDraw::Line(r, center + rotation * -field.extents, center + rotation * field.extents, warn);
            renderer::DebugDraw::Line(r,
                center + rotation * math::Vector3{ -field.extents.x, field.extents.y, -field.extents.z },
                center + rotation * math::Vector3{ field.extents.x, -field.extents.y, field.extents.z }, warn);
        }
        return;
    }

    const bool global = field.radius <= 0.0f;
    const float glyphRadius = global ? kGlobalGlyphRadius : field.radius;
    if (global) {
        /// @note 全体に効く場は二重の小球で «境界なし» を示す。
        DrawDebugSphere(r, center, 0.15f, baseColor, dashed);
        DrawDebugSphere(r, center, 0.25f, baseColor, dashed);
    } else {
        DrawDebugSphere(r, center, field.radius, baseColor, dashed);
        const float guide = field.radius
            * (1.0f - std::pow(kFalloffGuideInfluence, 1.0f / field.falloffPower));
        if (guide > field.radius * 0.02f)
            DrawDebugSphere(r, center, guide, WithAlpha(baseColor, 0.4f), true);
    }

    const float sign = field.strength >= 0.0f ? 1.0f : -1.0f;
    const math::Vector4 glyphColor = WithAlpha(baseColor, field.strength == 0.0f ? 0.35f : 1.0f);
    switch (field.type) {
    case FlowFieldType::Uniform: {
        const float length = (std::max)(glyphRadius * 0.6f, 0.3f);
        const math::Vector3 dir = field.direction * sign;
        renderer::DebugDraw::Arrow(r, center - dir * (length * 0.5f), center + dir * (length * 0.5f),
                                   length * 0.2f, length * 0.07f, glyphColor);
        break;
    }
    case FlowFieldType::Sink:
    case FlowFieldType::Source: {
        const bool outward = (field.type == FlowFieldType::Source) == (sign > 0.0f);
        DrawRadialArrows(r, center, glyphRadius, outward, glyphColor);
        break;
    }
    case FlowFieldType::Vortex: {
        const float length = (std::max)(glyphRadius * 0.6f, 0.3f);
        renderer::DebugDraw::Line(r, center - field.direction * length, center + field.direction * length,
                                  WithAlpha(baseColor, 0.6f));
        DrawDebugSwirl(r, center, field.direction, glyphRadius * 0.5f, sign, glyphColor);
        break;
    }
    case FlowFieldType::Curl: {
        /// @note 向きを持たない乱流。渦を逆向きに 2 つ並べて «ばらばら» を示す。実際の向きは Flow Samples で見る。
        const math::Vector3 offset = DebugPerpendicular(math::Vector3::UP) * (glyphRadius * 0.25f);
        DrawDebugSwirl(r, center + offset, math::Vector3::UP, glyphRadius * 0.18f, 1.0f, glyphColor);
        DrawDebugSwirl(r, center - offset, math::Vector3::UP, glyphRadius * 0.18f, -1.0f, glyphColor);
        break;
    }
    case FlowFieldType::LegacyDrag:
    case FlowFieldType::Baked:
        break;
    }
}

} // namespace

std::string_view FlowFieldDebugPass::Name() const { return "FlowFieldDebug"; }

bool FlowFieldDebugPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.showFlowFields;
}

void FlowFieldDebugPass::Execute(PassResources&, RenderPassContext& ctx)
{
    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetViewProjection());
    for (EntityID id : ctx.scene.GetEntities<FlowField>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        const FlowField* component = ctx.scene.GetComponent<FlowField>(id);
        if (go == nullptr || component == nullptr || !go->activeInHierarchy()) continue;

        const float alpha = IsSelectedForDebug(*go, ctx) ? 1.0f : kUnselectedAlpha;
        const math::Quaternion& rotation = go->transform.worldRotation;
        for (const FlowFieldSettings& settings : component->forces) {
            if (!settings.enabled) continue;
            const ActiveFlowField field = ResolveFlowField(settings, go->transform.worldPosition, rotation);
            /// @note channels を絞った場は一部のエミッターにしか効かないので破線にする。
            const bool restricted = settings.channels != 0xFFFFFFFFu;
            DrawField(ctx.renderer, field, rotation, WithAlpha(FlowFieldColor(settings.fieldType), alpha),
                      restricted);
        }
    }
    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
