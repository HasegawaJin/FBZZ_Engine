/// @file    WorldPrimitiveRaycast.cpp
/// @brief   球・箱・カプセル・円柱とのレイ交差。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "WorldShapeRaycast.hpp"
#include <Math/Scalar/MathUtils.hpp>
#include <Physics/Collision/Shapes/CapsuleCollider.hpp>
#include <Physics/Collision/Shapes/CylinderCollider.hpp>
#include <Physics/Collision/Shapes/OBBCollider.hpp>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace fbzz::physics::world_query_detail
{
    using math::Vector3;

    namespace
    {
        /// @note 零半径や大座標で退化した法線は、MathContract を呼ぶ前に棄却する。
        bool NormalizeQueryNormal(const Vector3& unnormalized, Vector3& normal)
        {
            const float length = unnormalized.Length();
            if (!std::isfinite(length) || length < math::EPSILON) {
                return false;
            }
            normal = unnormalized / length;
            return true;
        }
    }

    bool RaySphere(const Vector3& origin, const Vector3& unitDirection, float maxDistance,
                   const Vector3& center, float radius, float& hitDistance, Vector3& hitNormal)
    {
        const Vector3 fromCenter = origin - center;
        // 正規化した方向に対する二次方程式は二次項が 1 なので、一次項の半分で解ける。
        const float halfLinear = Vector3::Dot(fromCenter, unitDirection);
        const float constant = Vector3::Dot(fromCenter, fromCenter) - radius * radius;
        const float discriminant = halfLinear * halfLinear - constant;
        if (discriminant < 0.0f) {
            return false;
        }

        const float rootOffset = std::sqrt(discriminant);
        float distance = -halfLinear - rootOffset;
        if (distance < 0.0f) {
            distance = -halfLinear + rootOffset;
        }
        if (distance < 0.0f || distance > maxDistance) {
            return false;
        }
        if (!std::isfinite(distance) || !NormalizeQueryNormal(origin + unitDirection * distance - center, hitNormal)) {
            return false;
        }

        hitDistance = distance;
        return true;
    }

    bool RayBounds(const Vector3& origin, const Vector3& unitDirection, float maxDistance,
                   const AABB& bounds, float& hitDistance, Vector3& hitNormal)
    {
        float entryDistance = 0.0f;
        float exitDistance = maxDistance;
        std::size_t entryAxis = 0;
        bool entersMaximumFace = false;

        const std::array originComponents{origin.x, origin.y, origin.z};
        const std::array directionComponents{unitDirection.x, unitDirection.y, unitDirection.z};
        const std::array minimum{bounds.min.x, bounds.min.y, bounds.min.z};
        const std::array maximum{bounds.max.x, bounds.max.y, bounds.max.z};

        // 各軸で箱に入ってから出るまでの区間を絞る。3 軸の区間が重ならなければ箱に当たらない。
        for (std::size_t axis = 0; axis < originComponents.size(); ++axis)
        {
            const float axisOrigin = originComponents[axis];
            const float axisDirection = directionComponents[axis];
            if (std::abs(axisDirection) < 1e-8f)
            {
                if (axisOrigin < minimum[axis] || axisOrigin > maximum[axis]) {
                    return false;
                }
                continue;
            }

            float axisEntry = (minimum[axis] - axisOrigin) / axisDirection;
            float axisExit = (maximum[axis] - axisOrigin) / axisDirection;
            const bool entersFromMaximum = axisEntry > axisExit;
            if (entersFromMaximum) {
                std::swap(axisEntry, axisExit);
            }
            if (axisEntry > entryDistance)
            {
                entryDistance = axisEntry;
                entryAxis = axis;
                entersMaximumFace = entersFromMaximum;
            }
            if (axisExit < exitDistance) {
                exitDistance = axisExit;
            }
            if (entryDistance > exitDistance) {
                return false;
            }
        }
        if (entryDistance < 0.0f || entryDistance > maxDistance) {
            return false;
        }

        // 距離 0 の境界ヒットでは進行方向によらず、始点が乗っている面の法線を使う。
        if (entryDistance == 0.0f)
        {
            for (std::size_t axis = 0; axis < originComponents.size(); ++axis)
            {
                if (originComponents[axis] != minimum[axis] && originComponents[axis] != maximum[axis]) {
                    continue;
                }
                entryAxis = axis;
                entersMaximumFace = originComponents[axis] == maximum[axis];
                break;
            }
        }

        hitDistance = entryDistance;
        std::array<float, 3> outwardNormal{};
        outwardNormal[entryAxis] = entersMaximumFace ? 1.0f : -1.0f;
        hitNormal = {outwardNormal[0], outwardNormal[1], outwardNormal[2]};
        return true;
    }

    bool RayOrientedBox(const Vector3& origin, const Vector3& unitDirection, float maxDistance,
                        const OBBCollider& box, float& hitDistance, Vector3& hitNormal)
    {
        const math::Quaternion worldRotation = box.GetRotation();
        const math::Quaternion inverseRotation = worldRotation.Inverse();
        const Vector3 localOrigin = inverseRotation * (origin - box.GetCenter());
        const Vector3 localDirection = inverseRotation * unitDirection;
        const AABB localBounds{-box.m_halfExtents, box.m_halfExtents};

        float distance;
        Vector3 localNormal;
        if (!RayBounds(localOrigin, localDirection, maxDistance, localBounds, distance, localNormal)) {
            return false;
        }
        if (!NormalizeQueryNormal(worldRotation * localNormal, hitNormal)) {
            return false;
        }
        hitDistance = distance;
        return true;
    }

    bool RayCapsule(const Vector3& origin, const Vector3& unitDirection, float maxDistance,
                    const CapsuleCollider& capsule, float& hitDistance, Vector3& hitNormal)
    {
        const Vector3 segmentStart = capsule.GetSegmentStart();
        const Vector3 segmentEnd = capsule.GetSegmentEnd();
        const float radius = capsule.m_radius;
        const Vector3 segment = segmentEnd - segmentStart;
        const Vector3 fromSegmentStart = origin - segmentStart;
        const float segmentLengthSquared = Vector3::Dot(segment, segment);

        // 軸に垂直な平面へ投影して、無限円柱の側面との交差を解く。
        const float directionAlongSegment = Vector3::Dot(unitDirection, segment) / segmentLengthSquared;
        const float originAlongSegment = Vector3::Dot(fromSegmentStart, segment) / segmentLengthSquared;
        const Vector3 radialDirection = unitDirection - segment * directionAlongSegment;
        const Vector3 radialOrigin = fromSegmentStart - segment * originAlongSegment;
        const float quadratic = Vector3::Dot(radialDirection, radialDirection);
        const float linear = 2.0f * Vector3::Dot(radialDirection, radialOrigin);
        const float constant = Vector3::Dot(radialOrigin, radialOrigin) - radius * radius;

        float nearestDistance = std::numeric_limits<float>::infinity();
        Vector3 nearestNormal;
        if (std::abs(quadratic) > 1e-8f)
        {
            const float discriminant = linear * linear - 4.0f * quadratic * constant;
            if (discriminant >= 0.0f)
            {
                const float rootOffset = std::sqrt(discriminant);
                float distance = (-linear - rootOffset) / (2.0f * quadratic);
                if (distance < 0.0f) {
                    distance = (-linear + rootOffset) / (2.0f * quadratic);
                }
                if (distance >= 0.0f && distance < maxDistance)
                {
                    const float segmentFraction = directionAlongSegment * distance + originAlongSegment;
                    if (segmentFraction >= 0.0f && segmentFraction <= 1.0f)
                    {
                        const Vector3 intersection = origin + unitDirection * distance;
                        const Vector3 pointOnAxis = segmentStart + segment * segmentFraction;
                        Vector3 candidateNormal;
                        if (NormalizeQueryNormal(intersection - pointOnAxis, candidateNormal))
                        {
                            nearestDistance = distance;
                            nearestNormal = candidateNormal;
                        }
                    }
                }
            }
        }

        // 側面の候補に両端の球を加え、最も近い候補の距離と法線を採用する。
        const auto considerEndSphere = [&](const Vector3& endpoint)
        {
            float candidateDistance;
            Vector3 candidateNormal;
            if (RaySphere(origin, unitDirection, maxDistance, endpoint, radius, candidateDistance, candidateNormal)
                && candidateDistance < nearestDistance)
            {
                nearestDistance = candidateDistance;
                nearestNormal = candidateNormal;
            }
        };
        considerEndSphere(segmentStart);
        considerEndSphere(segmentEnd);

        if (!std::isfinite(nearestDistance) || nearestDistance > maxDistance) {
            return false;
        }
        hitDistance = nearestDistance;
        hitNormal = nearestNormal;
        return true;
    }

    bool RayCylinder(const Vector3& origin, const Vector3& unitDirection, float maxDistance,
                     const CylinderCollider& cylinder, float& hitDistance, Vector3& hitNormal)
    {
        const Vector3 center = cylinder.GetCenter();
        const Vector3 axis = cylinder.GetAxis();
        const float radius = cylinder.m_radius;
        const float halfHeight = cylinder.m_halfHeight;

        const Vector3 fromCenter = origin - center;
        const float axialDirection = Vector3::Dot(unitDirection, axis);
        const float axialOrigin = Vector3::Dot(fromCenter, axis);
        const Vector3 radialDirection = unitDirection - axis * axialDirection;
        const Vector3 radialOrigin = fromCenter - axis * axialOrigin;

        float nearestDistance = std::numeric_limits<float>::infinity();
        Vector3 nearestNormal;

        // 無限円柱の解は近い順。上下の円板の間に収まる側面の交差だけを採る。
        const float quadratic = Vector3::Dot(radialDirection, radialDirection);
        if (quadratic > 1e-8f)
        {
            const float linear = 2.0f * Vector3::Dot(radialDirection, radialOrigin);
            const float constant = Vector3::Dot(radialOrigin, radialOrigin) - radius * radius;
            const float discriminant = linear * linear - 4.0f * quadratic * constant;
            if (discriminant >= 0.0f)
            {
                const float rootOffset = std::sqrt(discriminant);
                const float sideDistances[] = {(-linear - rootOffset) / (2.0f * quadratic),
                                               (-linear + rootOffset) / (2.0f * quadratic)};
                for (const float distance : sideDistances)
                {
                    if (distance < 0.0f || distance >= nearestDistance) {
                        continue;
                    }
                    const float axialDistance = axialOrigin + axialDirection * distance;
                    if (std::abs(axialDistance) > halfHeight) {
                        continue;
                    }
                    Vector3 candidateNormal;
                    if (NormalizeQueryNormal(origin + unitDirection * distance - (center + axis * axialDistance),
                                             candidateNormal))
                    {
                        nearestDistance = distance;
                        nearestNormal = candidateNormal;
                        break;
                    }
                }
            }
        }

        // 円板の平面までの距離を求め、半径内かつ側面より手前にある交差だけを採用する。
        if (std::abs(axialDirection) > 1e-8f)
        {
            constexpr float CAP_DIRECTIONS[] = {-1.0f, 1.0f};
            for (const float capDirection : CAP_DIRECTIONS)
            {
                const float capHeight = halfHeight * capDirection;
                const float distance = (capHeight - axialOrigin) / axialDirection;
                if (distance < 0.0f || distance >= nearestDistance) {
                    continue;
                }
                if ((origin + unitDirection * distance - (center + axis * capHeight)).LengthSq() > radius * radius) {
                    continue;
                }
                nearestDistance = distance;
                nearestNormal = axis * capDirection;
            }
        }

        if (!std::isfinite(nearestDistance) || nearestDistance > maxDistance) {
            return false;
        }
        hitDistance = nearestDistance;
        hitNormal = nearestNormal;
        return true;
    }
}
