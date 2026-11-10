/// @file    Mesh.cpp
/// @brief   Mesh ユーティリティ関数。
/// @author  Hasegawa Jin
/// @date    2026-05-31
#include "Graphics/Renderer/Mesh.hpp"
#include <cmath>
#include <limits>
#include <algorithm>
#include <array>
#include <atomic>
#include <map>

namespace fbzz::renderer {
namespace {

/// @note A closed, consistently oriented surface around the center contains the ball bounded by its nearest face plane.
/// @note Exact position welding joins UV/normal seams; missing, reversed, degenerate or nonmanifold faces reject the proof.
/// @see https://www.pbr-book.org/4ed/Shapes/Basic_Shape_Interface Closed surface orientation and intersection contracts.
float ComputeInteriorRadius(const Mesh& mesh)
{
    constexpr size_t MAX_INDICES = 6144;
    if (mesh.isSkinned || !mesh.cpuSkinnedVertices.empty() || !mesh.morphTargets.empty()
        || mesh.cpuIndices.empty() || mesh.cpuIndices.size() > MAX_INDICES
        || mesh.cpuIndices.size() % 3 != 0 || mesh.indexCount != mesh.cpuIndices.size()) return 0;
    const auto center = mesh.boundsCenter;
    if (!std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(center.z)
        || !std::isfinite(mesh.boundsRadius) || mesh.boundsRadius <= 0) return 0;
    std::map<std::array<float, 3>, uint32_t> welded;
    struct Edge { int count = 0; int direction = 0; };
    std::map<std::array<uint32_t, 2>, Edge> edges;
    double radius = mesh.boundsRadius;
    int orientation = 0;
    const double margin = (std::max)(1.0, radius) * 1.0e-5;
    for (size_t i = 0; i < mesh.cpuIndices.size(); i += 3) {
        std::array<math::Vector3, 3> p{};
        std::array<uint32_t, 3> ids{};
        for (size_t j = 0; j < 3; ++j) {
            const auto index = mesh.cpuIndices[i + j];
            if (index >= mesh.cpuVertices.size()) return 0;
            p[j] = mesh.cpuVertices[index].position;
            if (!std::isfinite(p[j].x) || !std::isfinite(p[j].y) || !std::isfinite(p[j].z)) return 0;
            const double dx = double(p[j].x) - center.x, dy = double(p[j].y) - center.y, dz = double(p[j].z) - center.z;
            const double outer = double(mesh.boundsRadius) * (1.0 + 1.0e-6);
            if (dx*dx + dy*dy + dz*dz > outer*outer) return 0;
            ids[j] = welded.try_emplace({p[j].x, p[j].y, p[j].z}, static_cast<uint32_t>(welded.size())).first->second;
        }
        const std::array<double, 3> a{double(p[1].x) - p[0].x, double(p[1].y) - p[0].y, double(p[1].z) - p[0].z};
        const std::array<double, 3> b{double(p[2].x) - p[0].x, double(p[2].y) - p[0].y, double(p[2].z) - p[0].z};
        const std::array<double, 3> n{a[1]*b[2] - a[2]*b[1], a[2]*b[0] - a[0]*b[2], a[0]*b[1] - a[1]*b[0]};
        const double length = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
        if (!std::isfinite(length) || length <= 0) return 0;
        const double distance = (n[0]*(double(center.x) - p[0].x) + n[1]*(double(center.y) - p[0].y)
            + n[2]*(double(center.z) - p[0].z)) / length;
        if (!std::isfinite(distance) || std::abs(distance) <= margin) return 0;
        const int faceOrientation = distance > 0 ? 1 : -1;
        if (orientation != 0 && faceOrientation != orientation) return 0;
        orientation = faceOrientation;
        radius = (std::min)(radius, std::abs(distance));
        for (size_t j = 0; j < 3; ++j) {
            const auto from = ids[j], to = ids[(j + 1) % 3];
            if (from == to) return 0;
            auto& edge = edges[{(std::min)(from, to), (std::max)(from, to)}];
            if (++edge.count > 2) return 0;
            edge.direction += from < to ? 1 : -1;
        }
    }
    for (const auto& [key, edge] : edges) {
        if (edge.count != 2 || edge.direction != 0) return 0;
    }
    return static_cast<float>((std::max)(0.0, radius - margin));
}

} /// @note namespace

void Mesh::ComputeBounds()
{
    m_cpuGeometryRevision = 0;
    /// @note CPU 頂点配列 (スキンドメッシュ優先) から AABB と、中心からの最大距離 (球半径) を求める。
    /// @note Frustum は外接球を使い、遮蔽物の登録には別途証明した内接球だけを使う。
    occlusionRadius = 0;

    const bool hasSkinned = !cpuSkinnedVertices.empty();
    const bool hasStatic  = !cpuVertices.empty();

    if (!hasSkinned && !hasStatic) {
        boundsCenter  = {};
        boundsRadius  = 0.0f;
        boundsExtents = {};
        return;
    }

    math::Vector3 vmin = {  std::numeric_limits<float>::max(),
                             std::numeric_limits<float>::max(),
                             std::numeric_limits<float>::max() };
    math::Vector3 vmax = { -std::numeric_limits<float>::max(),
                            -std::numeric_limits<float>::max(),
                            -std::numeric_limits<float>::max() };

    auto expand = [&](const math::Vector3& p) {
        if (p.x < vmin.x) vmin.x = p.x;
        if (p.y < vmin.y) vmin.y = p.y;
        if (p.z < vmin.z) vmin.z = p.z;
        if (p.x > vmax.x) vmax.x = p.x;
        if (p.y > vmax.y) vmax.y = p.y;
        if (p.z > vmax.z) vmax.z = p.z;
    };

    if (hasSkinned) {
        for (const auto& v : cpuSkinnedVertices) expand(v.position);
    } else {
        for (const auto& v : cpuVertices)        expand(v.position);
    }

    boundsCenter  = (vmin + vmax) * 0.5f;
    boundsExtents = (vmax - vmin) * 0.5f;

    /// @note AABB 中心から最遠頂点までの距離を半径とする。
    float maxR2 = 0.0f;
    if (hasSkinned) {
        for (const auto& v : cpuSkinnedVertices) {
            const math::Vector3 d = v.position - boundsCenter;
            const float r2 = d.x*d.x + d.y*d.y + d.z*d.z;
            if (r2 > maxR2) maxR2 = r2;
        }
    } else {
        for (const auto& v : cpuVertices) {
            const math::Vector3 d = v.position - boundsCenter;
            const float r2 = d.x*d.x + d.y*d.y + d.z*d.z;
            if (r2 > maxR2) maxR2 = r2;
        }
    }
    boundsRadius = std::sqrt(maxR2);
    occlusionRadius = ComputeInteriorRadius(*this);
}

void Mesh::ComputeBoundsExtents()
{
    m_cpuGeometryRevision = 0;
    occlusionRadius = 0;
    const bool hasSkinned = !cpuSkinnedVertices.empty();
    const bool hasStatic  = !cpuVertices.empty();
    if (!hasSkinned && !hasStatic) {
        boundsExtents = {};
        return;
    }

    math::Vector3 vmin = {  std::numeric_limits<float>::max(),
                             std::numeric_limits<float>::max(),
                             std::numeric_limits<float>::max() };
    math::Vector3 vmax = { -std::numeric_limits<float>::max(),
                            -std::numeric_limits<float>::max(),
                            -std::numeric_limits<float>::max() };
    const auto expand = [&](const math::Vector3& p) {
        if (p.x < vmin.x) vmin.x = p.x;
        if (p.y < vmin.y) vmin.y = p.y;
        if (p.z < vmin.z) vmin.z = p.z;
        if (p.x > vmax.x) vmax.x = p.x;
        if (p.y > vmax.y) vmax.y = p.y;
        if (p.z > vmax.z) vmax.z = p.z;
    };
    if (hasSkinned) {
        for (const auto& v : cpuSkinnedVertices) expand(v.position);
    } else {
        for (const auto& v : cpuVertices)        expand(v.position);
    }

    boundsExtents = (vmax - vmin) * 0.5f;
    occlusionRadius = ComputeInteriorRadius(*this);
}

void Mesh::PublishCpuGeometry()
{
    static std::atomic<uint64_t> nextRevision{1};
    m_cpuGeometryRevision = nextRevision.fetch_add(1, std::memory_order_relaxed);
}

} /// @note namespace fbzz::renderer
