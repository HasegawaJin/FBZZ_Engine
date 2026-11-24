/// @file    ColliderDebugGeometryDetail.hpp
/// @brief   形状別のデバッグジオメトリ生成と共通の線分追加。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#pragma once
#include <Physics/Debug/ColliderDebugGeometry.hpp>

namespace fbzz::physics
{
    class SphereCollider;
    class OBBCollider;
    class CapsuleCollider;
    class CylinderCollider;
    class ConvexHullCollider;
    struct Triangle;
    struct BVHTree;

    namespace collider_debug_detail
    {
        inline constexpr std::size_t MAX_MESH_DEBUG_LINES = 8192;

        inline void AppendLine(ColliderDebugGeometry& geometry, const math::Vector3& from, const math::Vector3& to)
        {
            geometry.lines.push_back({from, to});
        }

        inline void AppendTriangleEdges(ColliderDebugGeometry& geometry, const math::Vector3 (&vertices)[3])
        {
            /// @note BVH の詳細線では、残り予算が三角形一枚に満たなくても入る辺だけを出す。
            for (std::size_t edgeIndex = 0; edgeIndex < 3 && geometry.lines.size() < MAX_MESH_DEBUG_LINES; ++edgeIndex)
                AppendLine(geometry, vertices[edgeIndex], vertices[(edgeIndex + 1) % 3]);
        }

        void AppendBoundsBox(ColliderDebugGeometry& geometry, const math::Vector3& center,
                             const math::Vector3& halfExtents);
        ColliderDebugGeometry BuildBoundsGeometry(const Collider& collider);
        ColliderDebugGeometry BuildSphereGeometry(const SphereCollider& sphere);
        ColliderDebugGeometry BuildOBBGeometry(const OBBCollider& box);
        ColliderDebugGeometry BuildCapsuleGeometry(const CapsuleCollider& capsule);
        ColliderDebugGeometry BuildCylinderGeometry(const CylinderCollider& cylinder);
        ColliderDebugGeometry BuildMeshGeometry(const std::vector<Triangle>& triangles);
        ColliderDebugGeometry BuildConvexHullGeometry(const ConvexHullCollider& hull);
        ColliderDebugGeometry BuildBVHLodGeometry(const BVHTree& bvh, const ColliderDebugView& view);
    }
}
