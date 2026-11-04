/// @file    ScriptDebugProxies.cpp
/// @brief   Debug 領域の Script 転送と関連する内部ヘルパー。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include <Engine/Scene/Script.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Renderer/Gizmo.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <limits>
#include <type_traits>
#include <string_view>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fbzz::scene {
namespace {


/// @brief 要求 1 件の共通部分を埋める。フィールドの多重利用は ScriptDebugDrawType を参照。
ScriptDebugDrawCommand MakeDebugDrawCommand(ScriptDebugDrawType type, const math::Vector3& a,
                                            const math::Vector3& b, const math::Vector4& color,
                                            float duration, bool depthTest)
{
    ScriptDebugDrawCommand command;
    command.type      = type;
    command.a         = a;
    command.b         = b;
    command.color     = color;
    command.duration  = duration;
    command.depthTest = depthTest;
    return command;
}


} /// @note namespace
void ScriptDebugProxy::Log(std::string_view msg) const
{
    core::Logger::Info("%.*s", static_cast<int>(msg.size()), msg.data());
}


void ScriptDebugProxy::LogWarning(std::string_view msg) const
{
    core::Logger::Warn("%.*s", static_cast<int>(msg.size()), msg.data());
}


void ScriptDebugProxy::LogError(std::string_view msg) const
{
    core::Logger::Error("%.*s", static_cast<int>(msg.size()), msg.data());
}


void ScriptDebugProxy::DrawLine(const math::Vector3& a, const math::Vector3& b, const math::Vector4& color, float duration) const
{
    if (!script || !script->m_scene) return;
    script->m_scene->QueueScriptDebugDraw(
        MakeDebugDrawCommand(ScriptDebugDrawType::Line, a, b, color, duration, depthTest));
}


void ScriptDebugProxy::DrawSphere(const math::Vector3& center, float radius, const math::Vector4& color, float duration) const
{
    if (!script || !script->m_scene) return;
    ScriptDebugDrawCommand command =
        MakeDebugDrawCommand(ScriptDebugDrawType::Sphere, center, {}, color, duration, depthTest);
    command.radius = radius;
    script->m_scene->QueueScriptDebugDraw(command);
}


void ScriptDebugProxy::DrawBox(const math::Vector3& center, const math::Vector3& halfExtents, const math::Vector4& color, float duration) const
{
    if (!script || !script->m_scene) return;
    ScriptDebugDrawCommand command =
        MakeDebugDrawCommand(ScriptDebugDrawType::Box, center, {}, color, duration, depthTest);
    command.halfExtents = halfExtents;
    script->m_scene->QueueScriptDebugDraw(command);
}


void ScriptDebugProxy::DrawBox(const math::Vector3& center, const math::Vector3& halfExtents,
                               const math::Quaternion& rotation, const math::Vector4& color, float duration) const
{
    if (!script || !script->m_scene) return;
    ScriptDebugDrawCommand command =
        MakeDebugDrawCommand(ScriptDebugDrawType::OrientedBox, center, {}, color, duration, depthTest);
    command.halfExtents = halfExtents;
    command.rotation    = rotation;
    script->m_scene->QueueScriptDebugDraw(command);
}


void ScriptDebugProxy::DrawRay(const math::Vector3& origin, const math::Vector3& dir, const math::Vector4& color, float duration) const
{
    if (!script || !script->m_scene) return;
    script->m_scene->QueueScriptDebugDraw(
        MakeDebugDrawCommand(ScriptDebugDrawType::Ray, origin, origin + dir, color, duration, depthTest));
}


void ScriptDebugProxy::DrawArrow(const math::Vector3& from, const math::Vector3& to,
                                  float headLength, float headRadius,
                                  const math::Vector4& color, float duration) const
{
    if (!script || !script->m_scene) return;
    ScriptDebugDrawCommand command =
        MakeDebugDrawCommand(ScriptDebugDrawType::Arrow, from, to, color, duration, depthTest);
    command.radius      = headLength;
    command.halfExtents = { headRadius, 0.0f, 0.0f };
    script->m_scene->QueueScriptDebugDraw(command);
}


void ScriptDebugProxy::DrawCone(const math::Vector3& apex, const math::Vector3& direction,
                                 float height, float baseRadius,
                                 const math::Vector4& color, float duration) const
{
    if (!script || !script->m_scene) return;
    ScriptDebugDrawCommand command =
        MakeDebugDrawCommand(ScriptDebugDrawType::Cone, apex, direction, color, duration, depthTest);
    command.radius      = baseRadius;
    command.halfExtents = { height, 0.0f, 0.0f };
    script->m_scene->QueueScriptDebugDraw(command);
}


void ScriptDebugProxy::DrawCapsule(const math::Vector3& center, float radius, float halfHeight,
                                   const math::Quaternion& rotation, const math::Vector4& color,
                                   float duration) const
{
    if (!script || !script->m_scene) return;
    ScriptDebugDrawCommand command =
        MakeDebugDrawCommand(ScriptDebugDrawType::Capsule, center, {}, color, duration, depthTest);
    command.radius      = radius;
    command.halfExtents = { halfHeight, 0.0f, 0.0f };
    command.rotation    = rotation;
    script->m_scene->QueueScriptDebugDraw(command);
}


void ScriptDebugProxy::DrawCircle(const math::Vector3& center, const math::Vector3& normal, float radius,
                                  const math::Vector4& color, float duration) const
{
    if (!script || !script->m_scene) return;
    ScriptDebugDrawCommand command =
        MakeDebugDrawCommand(ScriptDebugDrawType::Circle, center, normal, color, duration, depthTest);
    command.radius = radius;
    script->m_scene->QueueScriptDebugDraw(command);
}


void ScriptDebugProxy::DrawArc(const math::Vector3& center, const math::Vector3& normal,
                               const math::Vector3& fromDirection, float radius, float angleDegrees,
                               const math::Vector4& color, float duration) const
{
    if (!script || !script->m_scene) return;
    ScriptDebugDrawCommand command =
        MakeDebugDrawCommand(ScriptDebugDrawType::Arc, center, normal, color, duration, depthTest);
    command.halfExtents = fromDirection;
    command.radius      = radius;
    command.angle       = math::ToRad(angleDegrees);
    script->m_scene->QueueScriptDebugDraw(command);
}


void ScriptDebugProxy::DrawPolyline(std::span<const math::Vector3> points, bool closed,
                                    const math::Vector4& color, float duration) const
{
    if (!script || !script->m_scene || points.size() < 2) return;
    for (size_t i = 0; i + 1 < points.size(); ++i)
        DrawLine(points[i], points[i + 1], color, duration);
    if (closed && points.size() > 2)
        DrawLine(points.back(), points.front(), color, duration);
}


#define GIZMO_ASSERT assert(renderer && "GizmoProxy is only valid inside OnDrawGizmos()")

void GizmoProxy::SetDepthTest(bool enabled) const
{
    GIZMO_ASSERT;
    renderer::DebugDraw::SetDepthTest(enabled);
}


void GizmoProxy::DrawCapsule(const math::Vector3& center, float radius, float halfHeight,
                             const math::Quaternion& rotation, const math::Vector4& color) const
{
    GIZMO_ASSERT;
    renderer::DebugDraw::Capsule(*renderer, center, radius, halfHeight, rotation, color);
}


void GizmoProxy::DrawCone(const math::Vector3& apex, const math::Vector3& direction,
                          float height, float baseRadius, const math::Vector4& color) const
{
    GIZMO_ASSERT;
    renderer::DebugDraw::Cone(*renderer, apex, direction, height, baseRadius, color);
}


void GizmoProxy::DrawCircle(const math::Vector3& center, const math::Vector3& normal, float radius,
                            const math::Vector4& color) const
{
    GIZMO_ASSERT;
    renderer::DebugDraw::Circle(*renderer, center, normal, radius, color);
}


void GizmoProxy::DrawArc(const math::Vector3& center, const math::Vector3& normal,
                         const math::Vector3& fromDirection, float radius, float angleDegrees,
                         const math::Vector4& color) const
{
    GIZMO_ASSERT;
    renderer::DebugDraw::Arc(*renderer, center, normal, fromDirection, radius,
                             math::ToRad(angleDegrees), color);
}


void GizmoProxy::DrawPolyline(std::span<const math::Vector3> points, bool closed,
                              const math::Vector4& color) const
{
    GIZMO_ASSERT;
    renderer::DebugDraw::Polyline(*renderer, points, closed, color);
}


void GizmoProxy::DrawTransformAxes(const math::Vector3& position,
                                    const math::Quaternion& rotation,
                                    float size) const
{
    GIZMO_ASSERT;
    renderer::Gizmo::TransformAxes(*renderer, position, rotation, size);
}


void GizmoProxy::DrawLine(const math::Vector3& from, const math::Vector3& to,
                           const math::Vector4& color) const
{
    GIZMO_ASSERT;
    renderer::DebugDraw::Line(*renderer, from, to, color);
}


void GizmoProxy::DrawArrow(const math::Vector3& from, const math::Vector3& to,
                             float headLength, float headRadius,
                             const math::Vector4& color) const
{
    GIZMO_ASSERT;
    renderer::DebugDraw::Arrow(*renderer, from, to, headLength, headRadius, color);
}


void GizmoProxy::DrawSphere(const math::Vector3& center, float radius,
                              const math::Vector4& color) const
{
    GIZMO_ASSERT;
    renderer::DebugDraw::Sphere(*renderer, center, radius, color);
}


void GizmoProxy::DrawBox(const math::Vector3& center, const math::Vector3& halfExtents,
                          const math::Vector4& color) const
{
    GIZMO_ASSERT;
    renderer::DebugDraw::Box(*renderer, center, halfExtents, color);
}


void GizmoProxy::DrawBox(const math::Vector3& center, const math::Vector3& halfExtents,
                          const math::Quaternion& rotation,
                          const math::Vector4& color) const
{
    GIZMO_ASSERT;
    renderer::DebugDraw::Box(*renderer, center, halfExtents, rotation, color);
}


void GizmoProxy::DrawSightCone(const math::Vector3& position,
                                 const math::Vector3& forward,
                                 float fovDegrees, float range,
                                 const math::Vector4& color) const
{
    GIZMO_ASSERT;
    renderer::Gizmo::SightCone(*renderer, position, forward, fovDegrees, range, color);
}


void GizmoProxy::DrawWaypointPath(std::span<const math::Vector3> waypoints,
                                    bool loop,
                                    const math::Vector4& color) const
{
    GIZMO_ASSERT;
    renderer::Gizmo::WaypointPath(*renderer, waypoints, loop, color);
}


void GizmoProxy::DrawDetectionRange(const math::Vector3& center,
                                      float innerRadius, float outerRadius,
                                      const math::Vector4& innerColor,
                                      const math::Vector4& outerColor) const
{
    GIZMO_ASSERT;
    renderer::Gizmo::DetectionRange(*renderer, center, innerRadius, outerRadius,
                                     innerColor, outerColor);
}


void GizmoProxy::DrawTargetLine(const math::Vector3& from, const math::Vector3& to,
                                  const math::Vector4& color) const
{
    GIZMO_ASSERT;
    renderer::Gizmo::TargetLine(*renderer, from, to, color);
}


} /// @note namespace fbzz::scene
