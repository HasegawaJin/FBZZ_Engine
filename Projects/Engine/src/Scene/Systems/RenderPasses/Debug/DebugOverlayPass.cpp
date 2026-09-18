/// @file    DebugOverlayPass.cpp
/// @brief   デバッグ線パスの共通部 (描き先の申告・選択判定・円や渦の線)。
/// @author  Hasegawa Jin
/// @date    2026-09-16
#include "DebugPasses.hpp"
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::scene {

namespace {

constexpr int   kCircleSegments = 32;
constexpr float kTwoPi          = 6.28318530f;

} // namespace

void DebugOverlayPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    builder.ReadWrite(m_target).SetAutoTarget(m_target);
}

bool IsSelectedForDebug(const GameObject& go, const RenderPassContext& ctx)
{
    const EntityID id = go.GetID();
    return id.IsValid() && ctx.settings.IsSelected(id.index, id.generation);
}

math::Vector3 DebugPerpendicular(const math::Vector3& axis)
{
    const math::Vector3 reference = std::abs(axis.y) < 0.9f ? math::Vector3::UP : math::Vector3::RIGHT;
    return math::Vector3::Cross(reference, axis).NormalizedOr(math::Vector3::RIGHT);
}

void DrawDebugCircle(renderer::IRenderer& r, const math::Vector3& center, const math::Vector3& axis,
                     float radius, const math::Vector4& color, bool dashed)
{
    if (radius <= 0.0f) return;
    const math::Vector3 u = DebugPerpendicular(axis);
    const math::Vector3 v = math::Vector3::Cross(axis, u);
    /// @note 破線 1 本の長さは円周から決める。固定長だと大きな球が点線の粒で埋まる。
    const float dashLength = (std::max)(radius * kTwoPi / (kCircleSegments * 2.0f), 0.03f);
    math::Vector3 prev = center + u * radius;
    for (int i = 1; i <= kCircleSegments; ++i) {
        const float angle = kTwoPi * static_cast<float>(i) / kCircleSegments;
        const math::Vector3 next = center + (u * std::cos(angle) + v * std::sin(angle)) * radius;
        if (!dashed)
            renderer::DebugDraw::Line(r, prev, next, color);
        else if (i % 2 == 0)
            renderer::DebugDraw::LineDashed(r, prev, next, color, dashLength, 2);
        prev = next;
    }
}

void DrawDebugSphere(renderer::IRenderer& r, const math::Vector3& center, float radius,
                     const math::Vector4& color, bool dashed)
{
    DrawDebugCircle(r, center, math::Vector3::UP, radius, color, dashed);
    DrawDebugCircle(r, center, math::Vector3::RIGHT, radius, color, dashed);
    DrawDebugCircle(r, center, math::Vector3::FORWARD, radius, color, dashed);
}

void DrawDebugSwirl(renderer::IRenderer& r, const math::Vector3& center, const math::Vector3& axis,
                    float radius, float sign, const math::Vector4& color)
{
    if (radius <= 0.0f || sign == 0.0f) return;
    constexpr int   kSegments = 24;
    constexpr float kSweep    = kTwoPi * 0.75f;
    const float direction = sign > 0.0f ? 1.0f : -1.0f;
    const math::Vector3 u = DebugPerpendicular(axis);
    const math::Vector3 v = math::Vector3::Cross(axis, u);
    const auto pointAt = [&](float angle) {
        return center + (u * std::cos(angle) + v * std::sin(angle)) * radius;
    };

    math::Vector3 prev = pointAt(0.0f);
    for (int i = 1; i <= kSegments; ++i) {
        const math::Vector3 next = pointAt(direction * kSweep * static_cast<float>(i) / kSegments);
        renderer::DebugDraw::Line(r, prev, next, color);
        prev = next;
    }
    /// @note 矢じりは弧の終端の接線方向。弧の弦へ向けると半径が小さいとき内側へ倒れて見える。
    const float endAngle = direction * kSweep;
    const math::Vector3 tangent = (v * std::cos(endAngle) - u * std::sin(endAngle)) * direction;
    const float head = radius * 0.3f;
    renderer::DebugDraw::Arrow(r, prev - tangent * head, prev, head, head * 0.35f, color);
}

} // namespace fbzz::scene
