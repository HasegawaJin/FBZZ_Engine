/// @file    VFXPathDebugPass.cpp
/// @brief   選択中の線状エフェクトの経路 (サンプル点) を折れ線で描く。
/// @author  Hasegawa Jin
/// @date    2026-09-16
#include "DebugPasses.hpp"
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/Components/MeshTrailComponent.hpp>
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/Components/TrailComponent.hpp>
#include <Engine/Scene/Components/VFXBeamComponent.hpp>
#include <Engine/Scene/Components/VFXLineComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <algorithm>

namespace fbzz::scene {

namespace {

constexpr math::Vector4 kPathColor   = { 0.35f, 1.00f, 1.00f, 1.0f };
constexpr math::Vector4 kSampleColor = { 1.00f, 1.00f, 1.00f, 1.0f };
constexpr math::Vector4 kEndColor    = { 1.00f, 0.45f, 0.85f, 1.0f };

math::Vector3 LocalToWorld(const Transform& tf, const math::Vector3& local)
{
    return tf.worldPosition + tf.worldRotation * math::Vector3{ local.x * tf.worldScale.x,
                                                                local.y * tf.worldScale.y,
                                                                local.z * tf.worldScale.z };
}

/// @brief 端点 2 つの間を segments 分割し、中点を sag だけ下げた放物線で points へ積む。
void AppendSaggedPath(std::vector<math::Vector3>& points, const math::Vector3& from,
                      const math::Vector3& to, int segments, float sag)
{
    const int count = std::clamp(segments, 1, 64);
    for (int i = 0; i <= count; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(count);
        math::Vector3 point = from + (to - from) * t;
        point.y -= sag * 4.0f * t * (1.0f - t);
        points.push_back(point);
    }
}

void DrawPath(RenderPassContext& ctx, const std::vector<math::Vector3>& points, bool closed)
{
    if (points.empty()) return;
    renderer::DebugDraw::Polyline(ctx.renderer, points, closed, kPathColor);
    const float distance = (points.front() - ctx.camera.m_position).Length();
    const float marker = std::clamp(distance * 0.004f, 0.005f, 0.1f);
    for (const math::Vector3& point : points)
        renderer::DebugDraw::Box(ctx.renderer, point, { marker, marker, marker }, kSampleColor);
    renderer::DebugDraw::Sphere(ctx.renderer, points.front(), marker * 3.0f, kEndColor);
}

} // namespace

std::string_view VFXPathDebugPass::Name() const { return "VFXPathDebug"; }

bool VFXPathDebugPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.showVFXPaths && !ctx.settings.selectedObjects.empty();
}

void VFXPathDebugPass::Execute(PassResources&, RenderPassContext& ctx)
{
    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetViewProjection());
    for (auto& go : ctx.scene.GameObjects()) {
        if (!go.activeInHierarchy() || !IsSelectedForDebug(go, ctx)) continue;
        const Transform& tf = go.transform;

        if (const auto* trail = go.GetComponent<TrailComponent>(); trail && trail->enabled) {
            m_points.clear();
            if (trail->beamMode) {
                const auto place = [&](const math::Vector3& p) {
                    return trail->beamWorldSpace ? p : LocalToWorld(tf, p);
                };
                m_points.push_back(place(trail->beamStart));
                for (const math::Vector3& p : trail->beamPoints) m_points.push_back(place(p));
                m_points.push_back(place(trail->beamEnd));
            } else if (!trail->pointBuffer.empty()) {
                /// @note リングバッファ。論理順は ringHead から ringCount 個 (ワールド座標)。
                const size_t capacity = trail->pointBuffer.size();
                const int count = std::clamp(trail->ringCount, 0, static_cast<int>(capacity));
                for (int i = 0; i < count; ++i)
                    m_points.push_back(
                        trail->pointBuffer[static_cast<size_t>(trail->ringHead + i) % capacity].position);
            }
            DrawPath(ctx, m_points, false);
        }

        if (const auto* meshTrail = go.GetComponent<MeshTrailComponent>(); meshTrail && !meshTrail->samples.empty()) {
            m_points.clear();
            const size_t capacity = meshTrail->samples.size();
            const int count = std::clamp(meshTrail->sampleCount, 0, static_cast<int>(capacity));
            for (int i = 0; i < count; ++i)
                m_points.push_back(
                    meshTrail->samples[static_cast<size_t>(meshTrail->sampleHead + i) % capacity].position);
            DrawPath(ctx, m_points, false);
        }

        if (const auto* line = go.GetComponent<LineRendererComponent>(); line && line->enabled) {
            m_points.clear();
            for (const math::Vector3& p : line->points)
                m_points.push_back(line->space == LineSpace::World ? p : LocalToWorld(tf, p));
            DrawPath(ctx, m_points, line->loop);
        }

        if (const auto* vfxLine = go.GetComponent<VFXLineComponent>(); vfxLine && vfxLine->enabled) {
            m_points.clear();
            const GameObject* fromObject = vfxLine->fromEntity.Resolve(ctx.scene);
            const GameObject* toObject   = vfxLine->toEntity.Resolve(ctx.scene);
            const math::Vector3 from = (fromObject ? fromObject->transform.worldPosition : tf.worldPosition)
                + vfxLine->fromOffset;
            const math::Vector3 to = toObject ? toObject->transform.worldPosition
                                              : LocalToWorld(tf, vfxLine->toPoint);
            const bool lightning = vfxLine->mode == VFXLineMode::Lightning;
            AppendSaggedPath(m_points, from, to, lightning ? 1 : vfxLine->beamSegments,
                             lightning ? 0.0f : vfxLine->sag);
            DrawPath(ctx, m_points, false);
        }

        if (const auto* beam = go.GetComponent<VFXBeamComponent>(); beam && beam->enabled) {
            m_points.clear();
            const GameObject* fromObject = beam->fromEntity.Resolve(ctx.scene);
            const GameObject* toObject   = beam->toEntity.Resolve(ctx.scene);
            const math::Vector3 from = (fromObject ? fromObject->transform.worldPosition : beam->fromPoint)
                + beam->fromOffset;
            const math::Vector3 to = (toObject ? toObject->transform.worldPosition : beam->toPoint)
                + beam->toOffset;
            AppendSaggedPath(m_points, from, to, beam->segments, beam->sag);
            DrawPath(ctx, m_points, false);
        }
    }
    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
