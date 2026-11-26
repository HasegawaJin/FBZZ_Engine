/// @file    ColliderDebugMesh.cpp
/// @brief   三角メッシュと高さ場の間引き、および凸包の共有辺を処理する。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "ColliderDebugGeometryDetail.hpp"
#include <Physics/Collision/BroadPhase/BVHNode.hpp>
#include <Physics/Collision/Shapes/ConvexHullCollider.hpp>
#include <algorithm>
#include <array>
#include <cstdint>

namespace fbzz::physics::collider_debug_detail
{
    namespace
    {
        constexpr std::size_t MAX_CONVEX_HULL_DEBUG_LINES = 2048;
        using EdgeKey = std::array<uint32_t, 2>;

        void AppendUniqueHullEdge(ColliderDebugGeometry& geometry,
                                  std::vector<EdgeKey>& emittedEdges,
                                  const std::vector<math::Vector3>& vertices,
                                  uint32_t startIndex, uint32_t endIndex)
        {
            if (geometry.lines.size() >= MAX_CONVEX_HULL_DEBUG_LINES) {
                return;
            }
            if (startIndex >= vertices.size() || endIndex >= vertices.size()) {
                return;
            }

            // 検索キーは無向。出力の向きは最初に出会った面の頂点順で決める。
            const EdgeKey key{std::min(startIndex, endIndex), std::max(startIndex, endIndex)};
            if (std::find(emittedEdges.begin(), emittedEdges.end(), key) != emittedEdges.end()) {
                return;
            }

            emittedEdges.push_back(key);
            AppendLine(geometry, vertices[startIndex], vertices[endIndex]);
        }
    }

    ColliderDebugGeometry BuildMeshGeometry(const std::vector<Triangle>& triangles)
    {
        ColliderDebugGeometry geometry;
        if (triangles.empty()) {
            return geometry;
        }

        constexpr std::size_t TRIANGLE_BUDGET = MAX_MESH_DEBUG_LINES / 3;
        geometry.lines.reserve(std::min(MAX_MESH_DEBUG_LINES, triangles.size() * 3));
        if (triangles.size() <= TRIANGLE_BUDGET)
        {
            for (const Triangle& triangle : triangles) {
                AppendTriangleEdges(geometry, triangle.v);
            }
        }
        else
        {
            // 視点を使わない表示は、入力順の全域から等間隔に選び、三角形単位で収める。
            for (std::size_t sampleIndex = 0; sampleIndex < TRIANGLE_BUDGET; ++sampleIndex)
            {
                const std::size_t triangleIndex = sampleIndex * triangles.size() / TRIANGLE_BUDGET;
                AppendTriangleEdges(geometry, triangles[triangleIndex].v);
            }
        }
        return geometry;
    }

    ColliderDebugGeometry BuildConvexHullGeometry(const ConvexHullCollider& hull)
    {
        ColliderDebugGeometry geometry;
        const auto& vertices = hull.GetWorldVertices();
        const auto& faces = hull.GetFaces();
        if (vertices.empty()) {
            return geometry;
        }
        if (faces.empty()) {
            return BuildBoundsGeometry(hull);
        }

        std::vector<EdgeKey> emittedEdges;
        emittedEdges.reserve(faces.size() * 3);
        geometry.lines.reserve(std::min(MAX_CONVEX_HULL_DEBUG_LINES, faces.size() * 3));
        constexpr std::size_t FACE_BUDGET = MAX_CONVEX_HULL_DEBUG_LINES / 3;
        const std::size_t faceStep = faces.size() > FACE_BUDGET
            ? (faces.size() + FACE_BUDGET - 1) / FACE_BUDGET : 1;

        for (std::size_t faceIndex = 0;
             faceIndex < faces.size() && geometry.lines.size() < MAX_CONVEX_HULL_DEBUG_LINES;
             faceIndex += faceStep)
        {
            const auto& face = faces[faceIndex];
            AppendUniqueHullEdge(geometry, emittedEdges, vertices, face[0], face[1]);
            AppendUniqueHullEdge(geometry, emittedEdges, vertices, face[1], face[2]);
            AppendUniqueHullEdge(geometry, emittedEdges, vertices, face[2], face[0]);
        }
        return geometry;
    }
}
