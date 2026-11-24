/// @file    WorldMeshRaycast.cpp
/// @brief   BVH の三角形と凸包面からレイの最近接ヒットを探す。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "WorldShapeRaycast.hpp"
#include <Physics/Collision/BroadPhase/BVHNode.hpp>
#include <Physics/Collision/Shapes/ConvexHullCollider.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace fbzz::physics::world_query_detail
{
    using math::Vector3;

    namespace
    {
        /// @note Möller–Trumbore。負の距離はヒットなしとして扱う。
        float RayTriangleDistance(const Vector3& origin, const Vector3& unitDirection,
                                  const Vector3& vertexA, const Vector3& vertexB, const Vector3& vertexC)
        {
            constexpr float PARALLEL_TOLERANCE = 1e-7f;
            const Vector3 edgeAB = vertexB - vertexA;
            const Vector3 edgeAC = vertexC - vertexA;
            const Vector3 directionCrossAC = Vector3::Cross(unitDirection, edgeAC);
            const float determinant = Vector3::Dot(edgeAB, directionCrossAC);
            if (std::abs(determinant) < PARALLEL_TOLERANCE) return -1.0f;

            const float inverseDeterminant = 1.0f / determinant;
            const Vector3 fromVertexA = origin - vertexA;
            const float weightB = inverseDeterminant * Vector3::Dot(fromVertexA, directionCrossAC);
            if (weightB < 0.0f || weightB > 1.0f) return -1.0f;

            const Vector3 originCrossAB = Vector3::Cross(fromVertexA, edgeAB);
            const float weightC = inverseDeterminant * Vector3::Dot(unitDirection, originCrossAB);
            if (weightC < 0.0f || weightB + weightC > 1.0f) return -1.0f;
            return inverseDeterminant * Vector3::Dot(edgeAC, originCrossAB);
        }

        Vector3 NormalAgainstRay(const Vector3& faceNormal, const Vector3& unitDirection)
        {
            return Vector3::Dot(faceNormal, unitDirection) < 0.0f ? faceNormal : -faceNormal;
        }
    }

    bool RayTriangleTree(const Vector3& origin, const Vector3& unitDirection, float maxDistance,
                         const BVHTree& tree, float& hitDistance, Vector3& hitNormal)
    {
        const Vector3 rayEnd = origin + unitDirection * maxDistance;
        const AABB queryBounds{
            {std::min(origin.x, rayEnd.x), std::min(origin.y, rayEnd.y), std::min(origin.z, rayEnd.z)},
            {std::max(origin.x, rayEnd.x), std::max(origin.y, rayEnd.y), std::max(origin.z, rayEnd.z)}
        };
        float nearestDistance = std::numeric_limits<float>::infinity();
        Vector3 nearestNormal;

        tree.Query(queryBounds, [&](const Triangle& triangle)
        {
            const float distance = RayTriangleDistance(origin, unitDirection,
                                                       triangle.v[0], triangle.v[1], triangle.v[2]);
            if (distance > 0.0f && distance < nearestDistance)
            {
                nearestDistance = distance;
                nearestNormal = NormalAgainstRay(triangle.normal, unitDirection);
            }
        });

        if (!std::isfinite(nearestDistance) || nearestDistance > maxDistance) return false;
        hitDistance = nearestDistance;
        hitNormal = nearestNormal;
        return true;
    }

    bool RayConvexHull(const Vector3& origin, const Vector3& unitDirection, float maxDistance,
                       const ConvexHullCollider& hull, float& hitDistance, Vector3& hitNormal)
    {
        const auto& vertices = hull.GetWorldVertices();
        float nearestDistance = std::numeric_limits<float>::infinity();
        Vector3 nearestNormal;

        for (const auto& face : hull.GetFaces())
        {
            const Vector3& vertexA = vertices[face[0]];
            const Vector3& vertexB = vertices[face[1]];
            const Vector3& vertexC = vertices[face[2]];
            const float distance = RayTriangleDistance(origin, unitDirection, vertexA, vertexB, vertexC);
            if (distance > 0.0f && distance < nearestDistance)
            {
                nearestDistance = distance;
                const Vector3 faceNormal = Vector3::Cross(vertexB - vertexA, vertexC - vertexA);
                if (faceNormal.LengthSq() > 1e-12f)
                    nearestNormal = NormalAgainstRay(faceNormal.Normalized(), unitDirection);
            }
        }

        if (!std::isfinite(nearestDistance) || nearestDistance > maxDistance) return false;
        hitDistance = nearestDistance;
        hitNormal = nearestNormal;
        return true;
    }
}
