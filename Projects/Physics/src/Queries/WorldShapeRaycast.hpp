/// @file    WorldShapeRaycast.hpp
/// @brief   World の通常レイと膨張レイが使う形状別の交差判定。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#pragma once
#include <Math/LinearAlgebra/Vector3.hpp>

namespace fbzz::physics
{
    struct AABB;
    struct BVHTree;
    class OBBCollider;
    class CapsuleCollider;
    class CylinderCollider;
    class ConvexHullCollider;

    namespace world_query_detail
    {
        bool RaySphere(const math::Vector3& origin, const math::Vector3& unitDirection, float maxDistance,
                       const math::Vector3& center, float radius,
                       float& hitDistance, math::Vector3& hitNormal);
        bool RayBounds(const math::Vector3& origin, const math::Vector3& unitDirection, float maxDistance,
                       const AABB& bounds, float& hitDistance, math::Vector3& hitNormal);
        bool RayOrientedBox(const math::Vector3& origin, const math::Vector3& unitDirection, float maxDistance,
                            const OBBCollider& box, float& hitDistance, math::Vector3& hitNormal);
        bool RayCapsule(const math::Vector3& origin, const math::Vector3& unitDirection, float maxDistance,
                        const CapsuleCollider& capsule, float& hitDistance, math::Vector3& hitNormal);
        bool RayCylinder(const math::Vector3& origin, const math::Vector3& unitDirection, float maxDistance,
                         const CylinderCollider& cylinder, float& hitDistance, math::Vector3& hitNormal);
        bool RayTriangleTree(const math::Vector3& origin, const math::Vector3& unitDirection, float maxDistance,
                             const BVHTree& tree, float& hitDistance, math::Vector3& hitNormal);
        bool RayConvexHull(const math::Vector3& origin, const math::Vector3& unitDirection, float maxDistance,
                           const ConvexHullCollider& hull, float& hitDistance, math::Vector3& hitNormal);
    }
}
