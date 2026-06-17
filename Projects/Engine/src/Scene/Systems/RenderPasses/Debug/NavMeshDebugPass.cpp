// FBZZ Engine
// NavMeshDebugPass.cpp | fbzz::scene
// NavMesh ポリゴン・エージェントパス・センサー視野角を HDR バッファへ描画する IRenderPass 実装
#include "DebugPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/NavMeshSurfaceComponent.hpp>
#include <Engine/Scene/Components/NavMeshAgentComponent.hpp>
#include <Engine/Scene/Components/NavMeshPatrolComponent.hpp>
#include <Engine/Scene/Components/NavMeshSensorComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

namespace fbzz::scene {

std::string_view NavMeshDebugPass::Name() const { return "NavMeshDebug"; }

std::vector<renderer::RenderGraph::ResourceAccess> NavMeshDebugPass::DeclareAccesses(const RenderPassContext&) const
{
    using U = renderer::RenderGraph::ResourceUsage;
    return { { "HDR", U::ReadWrite } };
}

namespace {

// origin を頂点として forward 方向中心に angleDeg の扇形ワイヤーを distance まで描く。
// WHY: 視野範囲を直感的に把握できる「扇形」は 3D 円錐 (DebugDraw::Cone) ではなく
//      XZ 平面上の扁平な扇のほうが分かりやすいため、Line の組み合わせで自作する。
void DrawVisionFan(renderer::IRenderer& renderer, const math::Vector3& origin,
                   const math::Vector3& forward, float angleDeg, float distance,
                   const math::Vector4& color, int segments = 20)
{
    math::Vector3 fwd = forward;
    fwd.y = 0.0f;
    if (fwd.LengthSq() < 0.0001f) fwd = math::Vector3::FORWARD;
    fwd = fwd.Normalized();

    const float halfAngleRad = std::clamp(angleDeg, 1.0f, 360.0f) * 0.5f * (3.14159265f / 180.0f);

    // Y 軸回りの回転 (左手系: +Z が前方)。
    auto rotateY = [](const math::Vector3& v, float rad) {
        const float c = std::cos(rad), s = std::sin(rad);
        return math::Vector3{ v.x * c + v.z * s, v.y, -v.x * s + v.z * c };
    };

    const math::Vector3 leftEdge  = origin + rotateY(fwd,  halfAngleRad) * distance;
    const math::Vector3 rightEdge = origin + rotateY(fwd, -halfAngleRad) * distance;
    renderer::DebugDraw::Line(renderer, origin, leftEdge,  color);
    renderer::DebugDraw::Line(renderer, origin, rightEdge, color);

    math::Vector3 prev = leftEdge;
    for (int i = 1; i <= segments; ++i) {
        const float t = halfAngleRad - (2.0f * halfAngleRad) * (static_cast<float>(i) / segments);
        const math::Vector3 p = origin + rotateY(fwd, t) * distance;
        renderer::DebugDraw::Line(renderer, prev, p, color);
        prev = p;
    }
}

} // namespace

void NavMeshDebugPass::Execute(RenderPassContext& ctx)
{
    if (!ctx.settings.showNavMesh && !ctx.settings.showNavSensors) return;

    constexpr math::Vector4 kPolygonColor = { 0.2f, 0.6f, 1.0f, 1.0f };
    constexpr math::Vector4 kPathColor    = { 1.0f, 0.85f, 0.1f, 1.0f };
    constexpr math::Vector4 kSafeColor    = { 0.2f, 1.0f, 0.3f, 0.7f };
    constexpr math::Vector4 kDangerColor  = { 1.0f, 0.2f, 0.2f, 0.85f };

    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetViewProjection());

    if (ctx.settings.showNavMesh) {
        for (EntityID eid : ctx.scene.GetEntities<NavMeshSurfaceComponent>()) {
            auto* surface = ctx.scene.GetComponent<NavMeshSurfaceComponent>(eid);
            auto* go      = ctx.scene.GetGameObject(eid);
            if (!surface || !go) continue;

            if (surface->needsBake || !surface->navMesh.IsValid()) {
                const math::Vector4 pendingColor = { 1.0f, 0.55f, 0.05f, 0.85f };
                if (surface->collectObjects == NavMeshCollectObjects::ThisObject) {
                    if (auto* tc = ctx.scene.GetComponent<TerrainComponent>(eid)) {
                        const float hw = static_cast<float>(tc->columns - 1) * tc->cellSize * 0.5f;
                        const float hd = static_cast<float>(tc->rows    - 1) * tc->cellSize * 0.5f;
                        const math::Vector3 center = go->transform.worldPosition + math::Vector3(hw, 0.0f, hd);
                        renderer::DebugDraw::Box(ctx.renderer, center, { hw, tc->maxHeight * 0.5f, hd }, pendingColor);
                    } else {
                        renderer::DebugDraw::Box(ctx.renderer, go->transform.worldPosition, surface->size * 0.5f, pendingColor);
                    }
                } else {
                    renderer::DebugDraw::Box(ctx.renderer, go->transform.worldPosition, surface->size * 0.5f, pendingColor);
                }
            } else {
                constexpr math::Vector4 kFillColor = { 0.12f, 0.45f, 0.95f, 0.20f };
                if (surface->collectObjects == NavMeshCollectObjects::Volume) {
                    const math::Vector4 boundsColor = { kPolygonColor.x, kPolygonColor.y, kPolygonColor.z, 0.25f };
                    renderer::DebugDraw::Box(ctx.renderer, go->transform.worldPosition, surface->size * 0.5f, boundsColor);
                }
                // WHY: kLift を大きめに取ってテレイン面との z-fight を確実に回避する。
                constexpr float kLift = 0.08f;
                for (const auto& poly : surface->navMesh.polygons) {
                    const size_t n = poly.vertices.size();
                    std::vector<math::Vector3> lifted(n);
                    for (size_t j = 0; j < n; ++j)
                        lifted[j] = poly.vertices[j] + math::Vector3(0.f, kLift, 0.f);
                    renderer::DebugDraw::FilledPolygon(ctx.renderer, lifted.data(), n, kFillColor);
                    for (size_t i = 0; i < n; ++i)
                        renderer::DebugDraw::Line(ctx.renderer, lifted[i], lifted[(i + 1) % n], kPolygonColor);
                }
            }
        }

        constexpr math::Vector4 kStuckColor = { 1.0f, 0.15f, 0.1f, 1.0f };
        for (EntityID eid : ctx.scene.GetEntities<NavMeshAgentComponent>()) {
            auto* agent = ctx.scene.GetComponent<NavMeshAgentComponent>(eid);
            auto* go    = ctx.scene.GetGameObject(eid);
            if (!agent || !go) continue;
            if (agent->isStuck)
                renderer::DebugDraw::Sphere(ctx.renderer,
                    go->transform.worldPosition + math::Vector3::UP * 1.5f, 0.15f, kStuckColor);
            if (agent->path.empty()) continue;
            math::Vector3 prev = go->transform.worldPosition;
            for (size_t i = agent->currentWaypoint; i < agent->path.size(); ++i) {
                renderer::DebugDraw::Line(ctx.renderer, prev, agent->path[i], kPathColor);
                prev = agent->path[i];
            }
        }

        const math::Vector4 patrolColor = { kPathColor.x, kPathColor.y, kPathColor.z, 0.5f };
        for (EntityID eid : ctx.scene.GetEntities<NavMeshPatrolComponent>()) {
            auto* patrol = ctx.scene.GetComponent<NavMeshPatrolComponent>(eid);
            if (!patrol || patrol->waypoints.size() < 2) continue;
            const size_t count = patrol->mode == NavMeshPatrolComponent::Mode::LOOP
                ? patrol->waypoints.size() : patrol->waypoints.size() - 1;
            for (size_t i = 0; i < count; ++i) {
                const math::Vector3& from = patrol->waypoints[i];
                const math::Vector3& to   = patrol->waypoints[(i + 1) % patrol->waypoints.size()];
                renderer::DebugDraw::Line(ctx.renderer, from, to, patrolColor);
                renderer::DebugDraw::Sphere(ctx.renderer, from, 0.2f, patrolColor);
            }
        }
    }

    if (ctx.settings.showNavSensors) {
        for (EntityID eid : ctx.scene.GetEntities<NavMeshSensorComponent>()) {
            auto* sensor = ctx.scene.GetComponent<NavMeshSensorComponent>(eid);
            auto* go     = ctx.scene.GetGameObject(eid);
            if (!sensor || !go || !sensor->enabled) continue;
            const math::Vector4& color = sensor->targetVisible ? kDangerColor : kSafeColor;
            DrawVisionFan(ctx.renderer, go->transform.worldPosition, go->transform.forward,
                         sensor->viewAngleDeg, sensor->viewDistance, color);
            if (sensor->targetVisible) {
                if (auto* targetGo = ctx.scene.GetGameObject(sensor->detectedTarget))
                    renderer::DebugDraw::Line(ctx.renderer, go->transform.worldPosition,
                                              targetGo->transform.worldPosition, kDangerColor);
            }
        }
    }

    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
