/// @file    WorldSphereQueries.cpp
/// @brief   球との重なり判定と、形状を膨張させるスフィアキャストの近似。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "WorldQueryGeometry.hpp"
#include "WorldShapeRaycast.hpp"
#include <Physics/Collision/Shapes/CapsuleCollider.hpp>
#include <Physics/Collision/Shapes/CylinderCollider.hpp>
#include <Physics/Collision/Shapes/SphereCollider.hpp>
#include <algorithm>
#include <array>

namespace fbzz::physics::world_query_detail
{
    using math::Vector3;

    namespace
    {
        float DistanceSquaredToBounds(const Vector3& point, const AABB& bounds)
        {
            const std::array pointComponents{point.x, point.y, point.z};
            const std::array minimum{bounds.min.x, bounds.min.y, bounds.min.z};
            const std::array maximum{bounds.max.x, bounds.max.y, bounds.max.z};
            float distanceSquared = 0.0f;
            for (std::size_t axis = 0; axis < pointComponents.size(); ++axis)
            {
                const float coordinate = pointComponents[axis];
                if (coordinate < minimum[axis])
                {
                    const float gap = minimum[axis] - coordinate;
                    distanceSquared += gap * gap;
                }
                else if (coordinate > maximum[axis])
                {
                    const float gap = coordinate - maximum[axis];
                    distanceSquared += gap * gap;
                }
            }
            return distanceSquared;
        }

        bool SphereOverlapsCapsule(const Vector3& center, float radius, const CapsuleCollider& capsule)
        {
            const Vector3 segmentStart = capsule.GetSegmentStart();
            const Vector3 segment = capsule.GetSegmentEnd() - segmentStart;
            const Vector3 fromSegmentStart = center - segmentStart;
            const float segmentLengthSquared = Vector3::Dot(segment, segment);
            const float segmentFraction = segmentLengthSquared > 1e-8f
                ? std::clamp(Vector3::Dot(fromSegmentStart, segment) / segmentLengthSquared, 0.0f, 1.0f)
                : 0.0f;
            const Vector3 closestOnSegment = segmentStart + segment * segmentFraction;
            const float combinedRadius = radius + capsule.m_radius;
            return (center - closestOnSegment).LengthSq() <= combinedRadius * combinedRadius;
        }

        AABB ExpandedBounds(const Collider& collider, float expansionRadius)
        {
            const AABB bounds = collider.GetAABB();
            const Vector3 padding{expansionRadius, expansionRadius, expansionRadius};
            return {bounds.min - padding, bounds.max + padding};
        }

        void RaycastExpandedCapsule(const Vector3& origin, const Vector3& unitDirection, float maxDistance,
                                    const CapsuleCollider& capsule, float expansionRadius,
                                    float& hitDistance, Vector3& hitNormal)
        {
            const auto considerHit = [&](float candidateDistance, const Vector3& candidateNormal)
            {
                if (hitDistance < 0.0f || candidateDistance < hitDistance)
                {
                    hitDistance = candidateDistance;
                    hitNormal = candidateNormal;
                }
            };
            const float expandedRadius = capsule.m_radius + expansionRadius;
            float candidateDistance;
            Vector3 candidateNormal;
            if (RaySphere(origin, unitDirection, maxDistance, capsule.GetSegmentStart(), expandedRadius,
                          candidateDistance, candidateNormal)) {
                considerHit(candidateDistance, candidateNormal);
            }
            if (RaySphere(origin, unitDirection, maxDistance, capsule.GetSegmentEnd(), expandedRadius,
                          candidateDistance, candidateNormal)) {
                considerHit(candidateDistance, candidateNormal);
            }

            // 膨張した端球と外接箱のうち、最も手前のヒットを採る。
            const AABB bounds = ExpandedBounds(capsule, expansionRadius);
            if (RayBounds(origin, unitDirection, maxDistance, bounds, candidateDistance, candidateNormal)) {
                considerHit(candidateDistance, candidateNormal);
            }
        }
    }

    bool SphereOverlapsInstance(const Vector3& center, float radius, const ColliderInstance& instance)
    {
        if (!instance.collider) {
            return false;
        }
        const Collider& collider = *instance.collider;

        switch (collider.GetType())
        {
        case ColliderType::SPHERE:
        {
            const auto& sphere = static_cast<const SphereCollider&>(collider);
            const float combinedRadius = radius + sphere.m_radius;
            return (center - sphere.GetAABB().Center()).LengthSq() <= combinedRadius * combinedRadius;
        }
        case ColliderType::AABB:
        case ColliderType::OBB:
        case ColliderType::TRIANGLE_MESH:
        case ColliderType::HEIGHT_FIELD:
        case ColliderType::CONVEX_HULL:
            // 複雑な形状は外接箱で保守的に照会する。
            return DistanceSquaredToBounds(center, collider.GetAABB()) <= radius * radius;
        case ColliderType::CAPSULE:
            return SphereOverlapsCapsule(center, radius, static_cast<const CapsuleCollider&>(collider));
        case ColliderType::CYLINDER:
        {
            const auto& cylinder = static_cast<const CylinderCollider&>(collider);
            const Vector3 closestOnCylinder = cylinder.ClosestPoint(center);
            return (center - closestOnCylinder).LengthSq() <= radius * radius;
        }
        default:
            return false;
        }
    }

    void RaycastExpandedCollider(const Vector3& origin, const Vector3& unitDirection, float maxDistance,
                                 const ColliderInstance& instance, float expansionRadius,
                                 float& hitDistance, Vector3& hitNormal)
    {
        const Collider& collider = *instance.collider;
        switch (collider.GetType())
        {
        case ColliderType::SPHERE:
        {
            const auto& sphere = static_cast<const SphereCollider&>(collider);
            RaySphere(origin, unitDirection, maxDistance, sphere.GetAABB().Center(),
                      sphere.m_radius + expansionRadius, hitDistance, hitNormal);
            break;
        }
        case ColliderType::CAPSULE:
            RaycastExpandedCapsule(origin, unitDirection, maxDistance, static_cast<const CapsuleCollider&>(collider),
                                   expansionRadius, hitDistance, hitNormal);
            break;
        default:
        {
            const AABB bounds = ExpandedBounds(collider, expansionRadius);
            RayBounds(origin, unitDirection, maxDistance, bounds, hitDistance, hitNormal);
            break;
        }
        }
    }
}
