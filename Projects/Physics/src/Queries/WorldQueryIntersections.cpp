/// @file    WorldQueryIntersections.cpp
/// @brief   World のレイキャストをコライダー型に対応する交差判定へ振り分ける。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "WorldQueryGeometry.hpp"
#include "WorldShapeRaycast.hpp"
#include <Physics/Collision/Shapes/CapsuleCollider.hpp>
#include <Physics/Collision/Shapes/ConvexHullCollider.hpp>
#include <Physics/Collision/Shapes/CylinderCollider.hpp>
#include <Physics/Collision/Shapes/HeightFieldCollider.hpp>
#include <Physics/Collision/Shapes/OBBCollider.hpp>
#include <Physics/Collision/Shapes/SphereCollider.hpp>
#include <Physics/Collision/Shapes/TriangleMeshCollider.hpp>

namespace fbzz::physics::world_query_detail
{
    bool RaycastInstance(const math::Vector3& origin, const math::Vector3& unitDirection, float maxDistance,
                         const ColliderInstance& instance, float& hitDistance, math::Vector3& hitNormal)
    {
        if (!instance.collider) return false;
        const Collider& collider = *instance.collider;

        switch (collider.GetType())
        {
        case ColliderType::SPHERE:
        {
            const auto& sphere = static_cast<const SphereCollider&>(collider);
            return RaySphere(origin, unitDirection, maxDistance, sphere.GetAABB().Center(), sphere.m_radius,
                             hitDistance, hitNormal);
        }
        case ColliderType::AABB:
            return RayBounds(origin, unitDirection, maxDistance, collider.GetAABB(), hitDistance, hitNormal);
        case ColliderType::OBB:
            return RayOrientedBox(origin, unitDirection, maxDistance, static_cast<const OBBCollider&>(collider),
                                  hitDistance, hitNormal);
        case ColliderType::CAPSULE:
            return RayCapsule(origin, unitDirection, maxDistance, static_cast<const CapsuleCollider&>(collider),
                              hitDistance, hitNormal);
        case ColliderType::CYLINDER:
            return RayCylinder(origin, unitDirection, maxDistance, static_cast<const CylinderCollider&>(collider),
                               hitDistance, hitNormal);
        case ColliderType::TRIANGLE_MESH:
            return RayTriangleTree(origin, unitDirection, maxDistance,
                                   static_cast<const TriangleMeshCollider&>(collider).GetBVH(), hitDistance, hitNormal);
        case ColliderType::CONVEX_HULL:
            return RayConvexHull(origin, unitDirection, maxDistance, static_cast<const ConvexHullCollider&>(collider),
                                 hitDistance, hitNormal);
        case ColliderType::HEIGHT_FIELD:
            return RayTriangleTree(origin, unitDirection, maxDistance,
                                   static_cast<const HeightFieldCollider&>(collider).GetBVH(), hitDistance, hitNormal);
        default:
            return false;
        }
    }
}
