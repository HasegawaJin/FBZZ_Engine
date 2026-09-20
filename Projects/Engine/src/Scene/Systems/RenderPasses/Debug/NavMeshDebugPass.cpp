/// @file    NavMeshDebugPass.cpp
/// @brief   NavMesh・エージェント経路・センサー視野の診断描画を HDR バッファへ重ねるパス。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include "DebugPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
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

void NavMeshDebugPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    /// @note 描き先の束縛はフレームワークが行う (SetAutoTarget)。
    builder.ReadWrite("HDR").SetAutoTarget("HDR");
}

namespace {

using renderer::NavMeshDrawMode;

/// 面と z-fight しない最小の持ち上げ量と、外周エッジに立てる壁の高さ。
constexpr float kLift    = 0.06f;
constexpr float kRimRise = 0.30f;

/// Voxels 表示だけは 1 セル 1 ポリゴンになるため、他のモードより手前で打ち切る。
constexpr float kVoxelMaxDistance = 60.0f;
constexpr int   kVoxelCellBudget  = 12000;

constexpr math::Vector4 kPolyEdge     = { 0.35f, 0.68f, 1.00f, 0.30f };
constexpr math::Vector4 kBorderEdge   = { 1.00f, 0.58f, 0.12f, 0.95f };
constexpr math::Vector4 kPortalEdge   = { 0.25f, 1.00f, 0.85f, 0.90f };
constexpr math::Vector4 kPortalLink   = { 0.25f, 1.00f, 0.85f, 0.35f };
constexpr math::Vector4 kOffMeshColor = { 0.85f, 0.40f, 1.00f, 0.95f };
constexpr math::Vector4 kPathColor    = { 1.00f, 0.85f, 0.10f, 1.00f };
constexpr math::Vector4 kStuckColor   = { 1.00f, 0.15f, 0.10f, 1.00f };
constexpr math::Vector4 kSafeColor    = { 0.20f, 1.00f, 0.30f, 0.70f };
constexpr math::Vector4 kDangerColor  = { 1.00f, 0.20f, 0.20f, 0.85f };

/// areaType 別の塗り色。index は areaType & 7。0 は他のモードと同じ青に揃える。
constexpr math::Vector4 kAreaFill[8] = {
    { 0.16f, 0.52f, 0.95f, 0.40f },
    { 0.30f, 0.85f, 0.40f, 0.40f },
    { 0.95f, 0.75f, 0.20f, 0.40f },
    { 0.90f, 0.35f, 0.30f, 0.40f },
    { 0.75f, 0.40f, 0.95f, 0.40f },
    { 0.20f, 0.85f, 0.85f, 0.40f },
    { 0.95f, 0.50f, 0.75f, 0.40f },
    { 0.60f, 0.60f, 0.60f, 0.40f },
};

math::Vector4 VoxelColor(NavMeshBakeCell cell)
{
    switch (cell) {
    case NavMeshBakeCell::Walkable:    return { 0.20f, 0.75f, 0.35f, 0.40f };
    case NavMeshBakeCell::TooSteep:    return { 0.95f, 0.60f, 0.10f, 0.40f };
    case NavMeshBakeCell::TooHighStep: return { 0.90f, 0.35f, 0.85f, 0.45f };
    case NavMeshBakeCell::Obstructed:  return { 0.92f, 0.18f, 0.18f, 0.45f };
    case NavMeshBakeCell::Eroded:      return { 0.25f, 0.55f, 0.95f, 0.45f };
    default:                           return { 0.0f, 0.0f, 0.0f, 0.0f };
    }
}

/// @brief 線バッチが溢れる前に中間 Flush する。
/// @note バッチが満杯になると以降の Line() は黙って捨てられる。
void ReserveLines(size_t vertexCount)
{
    if (renderer::DebugDraw::PendingLineVertices() + vertexCount
        > renderer::DebugDraw::MaxBatchVertices())
        renderer::DebugDraw::Flush();
}

/// 塗りつぶしは線分とは別のバッチを使うので、頂点数も別に見る。
/// polygonVertices 個の凸ポリゴンは (n - 2) * 3 頂点を積む。
void ReserveFill(size_t polygonVertices)
{
    if (polygonVertices < 3) return;
    const size_t needed = (polygonVertices - 2) * 3;
    if (renderer::DebugDraw::PendingTriangleVertices() + needed
        > renderer::DebugDraw::MaxBatchVertices())
        renderer::DebugDraw::Flush();
}

/// @brief 外周エッジ (隣接ポリゴンを持たないエッジ) に低い壁を立てる。
/// @note 内部エッジと同じ 1px の線では NavMesh の「穴」と単なる分割線が同じ絵になるため、外周だけ手前へ立ち上げて輪郭を読めるようにする (Recast のデバッグ表示と同様)。
void DrawBorderRim(renderer::IRenderer& r, const math::Vector3& a, const math::Vector3& b)
{
    const math::Vector3 up = { 0.0f, kRimRise, 0.0f };
    ReserveLines(8);
    renderer::DebugDraw::Line(r, a, b, kBorderEdge);
    renderer::DebugDraw::Line(r, a + up, b + up, kBorderEdge);
    renderer::DebugDraw::Line(r, a, a + up, kBorderEdge);
    renderer::DebugDraw::Line(r, b, b + up, kBorderEdge);
}

bool EdgeHasPortal(const NavMeshPolygon& poly, const math::Vector3& a, const math::Vector3& b)
{
    /// @note 位置は Portal 構築時に同じ頂点配列からコピーされるので厳密一致で照合できる。
    for (const auto& portal : poly.portals) {
        if (portal.left.x == a.x && portal.left.z == a.z
         && portal.right.x == b.x && portal.right.z == b.z)
            return true;
    }
    return false;
}

/// from → to を上へ膨らませた円弧。Off-Mesh Link の「飛び移り」を 1 本の直線と区別する。
void DrawLinkArc(renderer::IRenderer& r, const math::Vector3& from, const math::Vector3& to,
                 const math::Vector4& color)
{
    constexpr int kSegments = 12;
    const float   span      = (to - from).Length();
    const float   rise      = std::clamp(span * 0.35f, 0.3f, 3.0f);
    ReserveLines(static_cast<size_t>(kSegments) * 2);
    math::Vector3 prev = from;
    for (int i = 1; i <= kSegments; ++i) {
        const float t = static_cast<float>(i) / kSegments;
        math::Vector3 p = from + (to - from) * t;
        p.y += std::sin(t * 3.14159265f) * rise;
        renderer::DebugDraw::Line(r, prev, p, color);
        prev = p;
    }
    renderer::DebugDraw::Sphere(r, from, 0.15f, color);
    renderer::DebugDraw::Sphere(r, to,   0.15f, color);
}

/// @brief origin を頂点として forward 方向中心に angleDeg の扇形ワイヤーを distance まで描く。
/// @note 視野範囲は 3D 円錐 (DebugDraw::Cone) でなく XZ 平面上の扁平な扇のほうが分かりやすいため、Line の組み合わせで自作する。
void DrawVisionFan(renderer::IRenderer& renderer, const math::Vector3& origin,
                   const math::Vector3& forward, float angleDeg, float distance,
                   const math::Vector4& color, int segments = 20)
{
    math::Vector3 fwd = forward;
    fwd.y = 0.0f;
    if (fwd.LengthSq() < 0.0001f) fwd = math::Vector3::FORWARD;
    fwd = fwd.Normalized();

    const float halfAngleRad = std::clamp(angleDeg, 1.0f, 360.0f) * 0.5f * (3.14159265f / 180.0f);

    /// @note Y 軸回りの回転 (左手系: +Z が前方)。
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

/// @brief ベイクが済んでいない / 失敗した Surface は、面の代わりに対象範囲の箱を出す。
/// @note 何も描かないと「ベイクしていない」と「ベイクしたが空だった」が同じ絵になる。
void DrawSurfacePlaceholder(RenderPassContext& ctx, EntityID eid,
                            const NavMeshSurfaceComponent& surface, const GameObject& go)
{
    math::Vector4 color = { 1.0f, 0.55f, 0.05f, 0.85f };
    if (surface.bakeState == NavMeshBakeState::Baking)      color = { 0.20f, 0.85f, 1.00f, 0.85f };
    else if (!surface.bakeStats.failReason.empty())         color = { 1.00f, 0.20f, 0.20f, 0.90f };

    if (surface.collectObjects == NavMeshCollectObjects::ThisObject) {
        if (auto* tc = ctx.scene.GetComponent<TerrainComponent>(eid)) {
            const float hw = static_cast<float>(tc->columns - 1) * tc->cellSize * 0.5f;
            const float hd = static_cast<float>(tc->rows    - 1) * tc->cellSize * 0.5f;
            const math::Vector3 center = go.transform.worldPosition + math::Vector3(hw, 0.0f, hd);
            renderer::DebugDraw::Box(ctx.renderer, center, { hw, tc->maxHeight * 0.5f, hd }, color);
            return;
        }
    }
    renderer::DebugDraw::Box(ctx.renderer, go.transform.worldPosition, surface.size * 0.5f, color);
}

void DrawPolygons(RenderPassContext& ctx, const NavMeshSurfaceComponent& surface,
                  NavMeshDrawMode mode, const math::Vector3& cameraPos, float maxDistanceSq)
{
    const NavMesh& navMesh = surface.navMesh;

    float fillAlpha = 0.40f;
    if (mode == NavMeshDrawMode::Transparent) fillAlpha = 0.14f;
    if (mode == NavMeshDrawMode::Portals)     fillAlpha = 0.08f;

    std::vector<math::Vector3> lifted;
    for (const auto& poly : navMesh.polygons) {
        const math::Vector3 center = poly.Center();
        if (maxDistanceSq > 0.0f && (center - cameraPos).LengthSq() > maxDistanceSq) continue;

        const size_t n = poly.vertices.size();
        if (n < 3) continue;

        lifted.resize(n);
        for (size_t i = 0; i < n; ++i)
            lifted[i] = poly.vertices[i] + math::Vector3(0.0f, kLift, 0.0f);

        math::Vector4 fill = (mode == NavMeshDrawMode::Areas)
            ? kAreaFill[poly.areaType & 7]
            : kAreaFill[0];
        fill.w = fillAlpha;
        ReserveFill(n);
        renderer::DebugDraw::FilledPolygon(ctx.renderer, lifted.data(), n, fill);

        for (size_t i = 0; i < n; ++i) {
            const math::Vector3& a = lifted[i];
            const math::Vector3& b = lifted[(i + 1) % n];
            if (EdgeHasPortal(poly, poly.vertices[i], poly.vertices[(i + 1) % n])) {
                if (mode == NavMeshDrawMode::Portals) {
                    ReserveLines(2);
                    renderer::DebugDraw::Line(ctx.renderer, a, b, kPortalEdge);
                } else if (mode != NavMeshDrawMode::Areas) {
                    ReserveLines(2);
                    renderer::DebugDraw::Line(ctx.renderer, a, b, kPolyEdge);
                }
            } else {
                DrawBorderRim(ctx.renderer, a, b);
            }
        }

        if (mode == NavMeshDrawMode::Portals) {
            const math::Vector3 from = center + math::Vector3(0.0f, kLift + 0.05f, 0.0f);
            for (const auto& portal : poly.portals) {
                if (portal.neighbor < 0
                 || portal.neighbor >= static_cast<int>(navMesh.polygons.size())) continue;
                const math::Vector3 to =
                    navMesh.polygons[static_cast<size_t>(portal.neighbor)].Center()
                    + math::Vector3(0.0f, kLift + 0.05f, 0.0f);
                ReserveLines(2);
                renderer::DebugDraw::Line(ctx.renderer, from, to, kPortalLink);
            }
        }
    }
}

void DrawVoxelGrid(RenderPassContext& ctx, const NavMeshBakeDebugGrid& grid,
                   const math::Vector3& cameraPos, float maxDistance)
{
    const float radius = std::min(maxDistance <= 0.0f ? kVoxelMaxDistance : maxDistance,
                                  kVoxelMaxDistance);
    const int   span   = std::max(1, static_cast<int>(radius / grid.cellSize));
    const int   centerX = static_cast<int>((cameraPos.x - grid.origin.x) / grid.cellSize);
    const int   centerZ = static_cast<int>((cameraPos.z - grid.origin.z) / grid.cellSize);

    const int minX = std::max(0, centerX - span);
    const int maxX = std::min(grid.columns - 1, centerX + span);
    const int minZ = std::max(0, centerZ - span);
    const int maxZ = std::min(grid.rows - 1, centerZ + span);

    int budget = kVoxelCellBudget;
    math::Vector3 quad[4];
    for (int z = minZ; z <= maxZ && budget > 0; ++z) {
        for (int x = minX; x <= maxX && budget > 0; ++x) {
            const NavMeshBakeCell state = grid.CellAt(x, z);
            if (state == NavMeshBakeCell::NoSurface) continue;

            const float h00 = grid.CornerAt(x,     z);
            const float h10 = grid.CornerAt(x + 1, z);
            const float h11 = grid.CornerAt(x + 1, z + 1);
            const float h01 = grid.CornerAt(x,     z + 1);
            /// @note 角が 1 つでも欠けているセルは高さが確定しないので描かない。
            if (h00 < -1.0e29f || h10 < -1.0e29f || h11 < -1.0e29f || h01 < -1.0e29f) continue;

            const float wx = grid.origin.x + x * grid.cellSize;
            const float wz = grid.origin.z + z * grid.cellSize;
            const float cs = grid.cellSize;
            quad[0] = { wx,      h00 + kLift, wz      };
            quad[1] = { wx + cs, h10 + kLift, wz      };
            quad[2] = { wx + cs, h11 + kLift, wz + cs };
            quad[3] = { wx,      h01 + kLift, wz + cs };
            ReserveFill(4);
            renderer::DebugDraw::FilledPolygon(ctx.renderer, quad, 4, VoxelColor(state));
            --budget;
        }
    }
}

} // namespace

bool NavMeshDebugPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.showNavMesh || ctx.settings.showNavSensors;
}

void NavMeshDebugPass::Execute(PassResources&, RenderPassContext& ctx)
{
    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetGpuViewProjection());

    if (ctx.settings.showNavMesh) {
        const NavMeshDrawMode mode        = ctx.settings.navMeshDrawMode;
        const math::Vector3   cameraPos   = ctx.camera.m_position;
        const float           drawDist    = ctx.settings.navMeshDrawDistance;
        const float           maxDistSq   = drawDist > 0.0f ? drawDist * drawDist : 0.0f;

        for (EntityID eid : ctx.scene.GetEntities<NavMeshSurfaceComponent>()) {
            auto* surface = ctx.scene.GetComponent<NavMeshSurfaceComponent>(eid);
            auto* go      = ctx.scene.GetGameObject(eid);
            /// @note 非アクティブな GO は NavigationSystem がベイクからも経路探索からも外す。
            ///       描画だけ残すと、実際には歩けない面が歩けるように見える。
            if (!surface || !go || !go->activeInHierarchy()) continue;

            if (surface->needsBake || !surface->navMesh.IsValid()) {
                DrawSurfacePlaceholder(ctx, eid, *surface, *go);
                continue;
            }

            if (surface->collectObjects == NavMeshCollectObjects::Volume) {
                constexpr math::Vector4 kBoundsColor = { 0.20f, 0.60f, 1.00f, 0.25f };
                renderer::DebugDraw::Box(ctx.renderer, go->transform.worldPosition,
                                         surface->size * 0.5f, kBoundsColor);
            }

            if (mode == NavMeshDrawMode::Voxels && surface->bakeDebug.IsValid())
                DrawVoxelGrid(ctx, surface->bakeDebug, cameraPos, drawDist);
            else
                DrawPolygons(ctx, *surface, mode, cameraPos, maxDistSq);

            for (const auto& link : surface->navMesh.offMeshLinks) {
                DrawLinkArc(ctx.renderer, link.posA, link.posB, kOffMeshColor);
                if (!link.bidirectional)
                    renderer::DebugDraw::Arrow(ctx.renderer, link.posA, link.posB,
                                               0.25f, 0.08f, kOffMeshColor);
            }
        }

        for (EntityID eid : ctx.scene.GetEntities<NavMeshAgentComponent>()) {
            auto* agent = ctx.scene.GetComponent<NavMeshAgentComponent>(eid);
            auto* go    = ctx.scene.GetGameObject(eid);
            if (!agent || !go || !go->activeInHierarchy()) continue;
            if (agent->isStuck)
                renderer::DebugDraw::Sphere(ctx.renderer,
                    go->transform.worldPosition + math::Vector3::UP * 1.5f, 0.15f, kStuckColor);
            if (agent->path.empty()) continue;
            math::Vector3 prev = go->transform.worldPosition;
            for (size_t i = agent->currentWaypoint; i < agent->path.size(); ++i) {
                ReserveLines(2);
                renderer::DebugDraw::Line(ctx.renderer, prev, agent->path[i], kPathColor);
                prev = agent->path[i];
            }
        }

        const math::Vector4 patrolColor = { kPathColor.x, kPathColor.y, kPathColor.z, 0.5f };
        for (EntityID eid : ctx.scene.GetEntities<NavMeshPatrolComponent>()) {
            auto* patrol = ctx.scene.GetComponent<NavMeshPatrolComponent>(eid);
            auto* go     = ctx.scene.GetGameObject(eid);
            if (!patrol || !go || !go->activeInHierarchy()) continue;
            if (patrol->waypoints.size() < 2) continue;
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
            if (!sensor || !go || !sensor->enabled || !go->activeInHierarchy()) continue;
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
