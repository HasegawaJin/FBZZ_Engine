// FBZZ Engine
// ColliderDebugGeometry.cpp | fbzz::physics
// コライダー可視化用のワイヤージオメトリ生成
#include <Physics/ColliderDebugGeometry.hpp>
#include <Physics/AABBCollider.hpp>
#include <Physics/OBBCollider.hpp>
#include <Physics/CapsuleCollider.hpp>
#include <Physics/ConvexHullCollider.hpp>
#include <Physics/CylinderCollider.hpp>
#include <Physics/SphereCollider.hpp>
#include <Physics/TriangleMeshCollider.hpp>
#include <Physics/HeightFieldCollider.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace fbzz::physics
{
namespace
{
    constexpr float PI = 3.14159265358979323846f;
    constexpr int CIRCLE_SEGMENTS = 24;
    constexpr size_t MAX_MESH_DEBUG_LINES = 8192;
    constexpr size_t MAX_CONVEX_HULL_DEBUG_LINES = 2048;

    void AddLine(ColliderDebugGeometry& out, const math::Vector3& from, const math::Vector3& to)
    {
        out.lines.push_back({ from, to });
    }

    bool CanAddLine(const ColliderDebugGeometry& out, size_t maxLines)
    {
        return out.lines.size() < maxLines;
    }

    void AddLineLimited(ColliderDebugGeometry& out,
                        const math::Vector3& from,
                        const math::Vector3& to,
                        size_t maxLines)
    {
        if (!CanAddLine(out, maxLines)) return;
        AddLine(out, from, to);
    }

    void AddTriangleEdgesLimited(ColliderDebugGeometry& out,
                                 const math::Vector3& a,
                                 const math::Vector3& b,
                                 const math::Vector3& c,
                                 size_t maxLines)
    {
        AddLineLimited(out, a, b, maxLines);
        AddLineLimited(out, b, c, maxLines);
        AddLineLimited(out, c, a, maxLines);
    }

    void AddBox(ColliderDebugGeometry& out, const math::Vector3& center, const math::Vector3& halfExtents)
    {
        const math::Vector3 h = halfExtents;
        const math::Vector3 c[8] = {
            center + math::Vector3{ -h.x, -h.y, -h.z },
            center + math::Vector3{  h.x, -h.y, -h.z },
            center + math::Vector3{  h.x,  h.y, -h.z },
            center + math::Vector3{ -h.x,  h.y, -h.z },
            center + math::Vector3{ -h.x, -h.y,  h.z },
            center + math::Vector3{  h.x, -h.y,  h.z },
            center + math::Vector3{  h.x,  h.y,  h.z },
            center + math::Vector3{ -h.x,  h.y,  h.z },
        };

        AddLine(out, c[0], c[1]); AddLine(out, c[1], c[2]);
        AddLine(out, c[2], c[3]); AddLine(out, c[3], c[0]);
        AddLine(out, c[4], c[5]); AddLine(out, c[5], c[6]);
        AddLine(out, c[6], c[7]); AddLine(out, c[7], c[4]);
        AddLine(out, c[0], c[4]); AddLine(out, c[1], c[5]);
        AddLine(out, c[2], c[6]); AddLine(out, c[3], c[7]);
    }

    void AddBox(ColliderDebugGeometry& out, const std::array<math::Vector3, 8>& c)
    {
        AddLine(out, c[0], c[1]); AddLine(out, c[1], c[2]);
        AddLine(out, c[2], c[3]); AddLine(out, c[3], c[0]);
        AddLine(out, c[4], c[5]); AddLine(out, c[5], c[6]);
        AddLine(out, c[6], c[7]); AddLine(out, c[7], c[4]);
        AddLine(out, c[0], c[4]); AddLine(out, c[1], c[5]);
        AddLine(out, c[2], c[6]); AddLine(out, c[3], c[7]);
    }

    void AddCircle(ColliderDebugGeometry& out,
                   const math::Vector3& center,
                   const math::Vector3& axisA,
                   const math::Vector3& axisB,
                   float radius)
    {
        for (int i = 0; i < CIRCLE_SEGMENTS; ++i)
        {
            const float a0 = (static_cast<float>(i) / CIRCLE_SEGMENTS) * PI * 2.0f;
            const float a1 = (static_cast<float>(i + 1) / CIRCLE_SEGMENTS) * PI * 2.0f;
            const math::Vector3 p0 = center
                + axisA * (std::cos(a0) * radius)
                + axisB * (std::sin(a0) * radius);
            const math::Vector3 p1 = center
                + axisA * (std::cos(a1) * radius)
                + axisB * (std::sin(a1) * radius);
            AddLine(out, p0, p1);
        }
    }

    void AddArc(ColliderDebugGeometry& out,
                const math::Vector3& center,
                const math::Vector3& axisA,
                const math::Vector3& axisB,
                float radius,
                float start,
                float end)
    {
        constexpr int ARC_SEGMENTS = CIRCLE_SEGMENTS / 2;
        for (int i = 0; i < ARC_SEGMENTS; ++i)
        {
            const float t0 = static_cast<float>(i) / ARC_SEGMENTS;
            const float t1 = static_cast<float>(i + 1) / ARC_SEGMENTS;
            const float a0 = start + (end - start) * t0;
            const float a1 = start + (end - start) * t1;
            const math::Vector3 p0 = center
                + axisA * (std::cos(a0) * radius)
                + axisB * (std::sin(a0) * radius);
            const math::Vector3 p1 = center
                + axisA * (std::cos(a1) * radius)
                + axisB * (std::sin(a1) * radius);
            AddLine(out, p0, p1);
        }
    }

    void BuildBasis(const math::Vector3& axis, math::Vector3& outX, math::Vector3& outY, math::Vector3& outZ)
    {
        outY = axis.Normalized();
        // 軸が UP に近いと外積が小さくなるため、参照軸を切り替えて安定した直交基底を作る。
        const math::Vector3 ref = std::abs(outY.y) > 0.95f ? math::Vector3::RIGHT : math::Vector3::UP;
        outX = math::Vector3::Cross(ref, outY).Normalized();
        outZ = math::Vector3::Cross(outY, outX).Normalized();
    }

    ColliderDebugGeometry BuildAABBGeometry(const Collider& collider)
    {
        ColliderDebugGeometry out;
        const AABB bounds = collider.GetAABB();
        AddBox(out, bounds.Center(), bounds.Extents());
        return out;
    }

    ColliderDebugGeometry BuildSphereGeometry(const SphereCollider& sphere)
    {
        ColliderDebugGeometry out;
        const math::Vector3 center = sphere.GetAABB().Center();
        AddCircle(out, center, math::Vector3::RIGHT, math::Vector3::UP, sphere.m_radius);
        AddCircle(out, center, math::Vector3::RIGHT, math::Vector3::FORWARD, sphere.m_radius);
        AddCircle(out, center, math::Vector3::UP, math::Vector3::FORWARD, sphere.m_radius);
        return out;
    }

    ColliderDebugGeometry BuildOBBGeometry(const OBBCollider& obb)
    {
        ColliderDebugGeometry out;
        AddBox(out, obb.GetCorners());
        return out;
    }

    ColliderDebugGeometry BuildCapsuleGeometry(const CapsuleCollider& capsule)
    {
        ColliderDebugGeometry out;
        const math::Vector3 top = capsule.GetSegmentEnd();
        const math::Vector3 bottom = capsule.GetSegmentStart();
        const math::Vector3 axis = top - bottom;
        if (axis.LengthSq() < 1e-6f)
            return BuildAABBGeometry(capsule);

        math::Vector3 x;
        math::Vector3 y;
        math::Vector3 z;
        BuildBasis(axis, x, y, z);

        AddCircle(out, top, x, z, capsule.m_radius);
        AddCircle(out, bottom, x, z, capsule.m_radius);
        AddArc(out, top, x, y, capsule.m_radius, 0.0f, PI);
        AddArc(out, top, z, y, capsule.m_radius, 0.0f, PI);
        AddArc(out, bottom, x, y, capsule.m_radius, PI, PI * 2.0f);
        AddArc(out, bottom, z, y, capsule.m_radius, PI, PI * 2.0f);

        AddLine(out, top + x * capsule.m_radius, bottom + x * capsule.m_radius);
        AddLine(out, top - x * capsule.m_radius, bottom - x * capsule.m_radius);
        AddLine(out, top + z * capsule.m_radius, bottom + z * capsule.m_radius);
        AddLine(out, top - z * capsule.m_radius, bottom - z * capsule.m_radius);
        return out;
    }

    ColliderDebugGeometry BuildCylinderGeometry(const CylinderCollider& cylinder)
    {
        ColliderDebugGeometry out;
        const math::Vector3 top = cylinder.GetSegmentEnd();
        const math::Vector3 bottom = cylinder.GetSegmentStart();
        const math::Vector3 axis = top - bottom;
        if (axis.LengthSq() < 1e-6f)
            return BuildAABBGeometry(cylinder);

        math::Vector3 x;
        math::Vector3 y;
        math::Vector3 z;
        BuildBasis(axis, x, y, z);

        AddCircle(out, top, x, z, cylinder.m_radius);
        AddCircle(out, bottom, x, z, cylinder.m_radius);

        AddLine(out, top + x * cylinder.m_radius, bottom + x * cylinder.m_radius);
        AddLine(out, top - x * cylinder.m_radius, bottom - x * cylinder.m_radius);
        AddLine(out, top + z * cylinder.m_radius, bottom + z * cylinder.m_radius);
        AddLine(out, top - z * cylinder.m_radius, bottom - z * cylinder.m_radius);
        return out;
    }

    ColliderDebugGeometry BuildTriangleMeshGeometry(const TriangleMeshCollider& mesh)
    {
        ColliderDebugGeometry out;
        const auto& triangles = mesh.GetBVH().triangles;
        if (triangles.empty()) return out;

        out.lines.reserve(std::min(MAX_MESH_DEBUG_LINES, triangles.size() * 3));
        const size_t lineBudget = MAX_MESH_DEBUG_LINES - (MAX_MESH_DEBUG_LINES % 3);
        if (triangles.size() * 3 <= lineBudget) {
            for (const Triangle& tri : triangles)
                AddTriangleEdgesLimited(out, tri.v[0], tri.v[1], tri.v[2], MAX_MESH_DEBUG_LINES);
            return out;
        }

        const size_t triangleBudget = std::max<size_t>(1, lineBudget / 3);
        for (size_t sample = 0; sample < triangleBudget; ++sample) {
            const size_t i = sample * triangles.size() / triangleBudget;
            const Triangle& tri = triangles[i];
            AddTriangleEdgesLimited(out, tri.v[0], tri.v[1], tri.v[2], MAX_MESH_DEBUG_LINES);
        }
        return out;
    }

    bool HasEdge(const std::vector<std::array<uint32_t, 2>>& edges, uint32_t a, uint32_t b)
    {
        if (a > b) std::swap(a, b);
        for (const auto& edge : edges) {
            if (edge[0] == a && edge[1] == b) return true;
        }
        return false;
    }

    void AddUniqueEdge(ColliderDebugGeometry& out,
                       std::vector<std::array<uint32_t, 2>>& edges,
                       const std::vector<math::Vector3>& vertices,
                       uint32_t a,
                       uint32_t b,
                       size_t maxLines)
    {
        if (!CanAddLine(out, maxLines)) return;
        if (a >= vertices.size() || b >= vertices.size()) return;
        uint32_t minIndex = a;
        uint32_t maxIndex = b;
        if (minIndex > maxIndex) std::swap(minIndex, maxIndex);
        if (HasEdge(edges, minIndex, maxIndex)) return;

        edges.push_back({ minIndex, maxIndex });
        AddLine(out, vertices[a], vertices[b]);
    }

    ColliderDebugGeometry BuildConvexHullGeometry(const ConvexHullCollider& hull)
    {
        ColliderDebugGeometry out;
        const auto& vertices = hull.GetWorldVertices();
        const auto& faces = hull.GetFaces();
        if (vertices.empty()) return out;

        if (faces.empty()) {
            return BuildAABBGeometry(hull);
        }

        std::vector<std::array<uint32_t, 2>> edges;
        edges.reserve(faces.size() * 3);
        out.lines.reserve(std::min(MAX_CONVEX_HULL_DEBUG_LINES, faces.size() * 3));
        const size_t faceBudget = std::max<size_t>(1, MAX_CONVEX_HULL_DEBUG_LINES / 3);
        const size_t step = faces.size() > faceBudget
            ? (faces.size() + faceBudget - 1) / faceBudget
            : 1;

        for (size_t i = 0; i < faces.size() && CanAddLine(out, MAX_CONVEX_HULL_DEBUG_LINES); i += step) {
            const auto& face = faces[i];
            AddUniqueEdge(out, edges, vertices, face[0], face[1], MAX_CONVEX_HULL_DEBUG_LINES);
            AddUniqueEdge(out, edges, vertices, face[1], face[2], MAX_CONVEX_HULL_DEBUG_LINES);
            AddUniqueEdge(out, edges, vertices, face[2], face[0], MAX_CONVEX_HULL_DEBUG_LINES);
        }
        return out;
    }
ColliderDebugGeometry BuildHeightFieldGeometry(const HeightFieldCollider& hf)
{
    ColliderDebugGeometry out;
    const auto& triangles = hf.GetBVH().triangles;
    if (triangles.empty()) return out;

    out.lines.reserve(std::min(MAX_MESH_DEBUG_LINES, triangles.size() * 3));
    const size_t lineBudget = MAX_MESH_DEBUG_LINES - (MAX_MESH_DEBUG_LINES % 3);
    if (triangles.size() * 3 <= lineBudget) {
        for (const Triangle& tri : triangles)
            AddTriangleEdgesLimited(out, tri.v[0], tri.v[1], tri.v[2], MAX_MESH_DEBUG_LINES);
        return out;
    }
    const size_t triangleBudget = std::max<size_t>(1, lineBudget / 3);
    for (size_t sample = 0; sample < triangleBudget; ++sample) {
        const size_t i = sample * triangles.size() / triangleBudget;
        AddTriangleEdgesLimited(out, triangles[i].v[0], triangles[i].v[1], triangles[i].v[2], MAX_MESH_DEBUG_LINES);
    }
    return out;
}

} // namespace

ColliderDebugGeometry BuildColliderDebugGeometry(const Collider& collider)
{
    switch (collider.GetType())
    {
    case ColliderType::SPHERE:
        return BuildSphereGeometry(static_cast<const SphereCollider&>(collider));
    case ColliderType::AABB:
        return BuildAABBGeometry(collider);
    case ColliderType::OBB:
        return BuildOBBGeometry(static_cast<const OBBCollider&>(collider));
    case ColliderType::CAPSULE:
        return BuildCapsuleGeometry(static_cast<const CapsuleCollider&>(collider));
    case ColliderType::CYLINDER:
        return BuildCylinderGeometry(static_cast<const CylinderCollider&>(collider));
    case ColliderType::TRIANGLE_MESH:
        return BuildTriangleMeshGeometry(static_cast<const TriangleMeshCollider&>(collider));
    case ColliderType::CONVEX_HULL:
        return BuildConvexHullGeometry(static_cast<const ConvexHullCollider&>(collider));
    case ColliderType::HEIGHT_FIELD:
        return BuildHeightFieldGeometry(static_cast<const HeightFieldCollider&>(collider));
    }

    return BuildAABBGeometry(collider);
}

} // namespace fbzz::physics
