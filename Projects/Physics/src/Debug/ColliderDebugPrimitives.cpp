/// @file    ColliderDebugPrimitives.cpp
/// @brief   プリミティブコライダーの箱・円・円弧・側線を生成する。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "ColliderDebugGeometryDetail.hpp"
#include <Physics/Collision/Shapes/CapsuleCollider.hpp>
#include <Physics/Collision/Shapes/CylinderCollider.hpp>
#include <Physics/Collision/Shapes/OBBCollider.hpp>
#include <Physics/Collision/Shapes/SphereCollider.hpp>
#include <array>
#include <cmath>

namespace fbzz::physics::collider_debug_detail
{
    namespace
    {
        constexpr float PI = 3.14159265358979323846f;
        constexpr int CIRCLE_SEGMENTS = 24;
        constexpr int ARC_SEGMENTS = CIRCLE_SEGMENTS / 2;

        struct AxialBasis
        {
            math::Vector3 radialAxisA; ///< 形状の長軸に直交する 1 本目の半径方向。
            math::Vector3 longitudinalAxis; ///< 円柱やカプセルの長さに沿う軸。
            math::Vector3 radialAxisB; ///< 長軸と radialAxisA に直交する 2 本目の半径方向。
        };

        void AppendBoxEdges(ColliderDebugGeometry& geometry, const std::array<math::Vector3, 8>& corners)
        {
            constexpr std::size_t EDGES[12][2] = {
                {0, 1}, {1, 2}, {2, 3}, {3, 0},
                {4, 5}, {5, 6}, {6, 7}, {7, 4},
                {0, 4}, {1, 5}, {2, 6}, {3, 7}
            };
            for (const auto& edge : EDGES) {
                AppendLine(geometry, corners[edge[0]], corners[edge[1]]);
            }
        }

        math::Vector3 CirclePoint(const math::Vector3& center,
                                  const math::Vector3& planeAxisA, const math::Vector3& planeAxisB,
                                  float radius, float angle)
        {
            return center + planeAxisA * (std::cos(angle) * radius)
                          + planeAxisB * (std::sin(angle) * radius);
        }

        void AppendCircle(ColliderDebugGeometry& geometry, const math::Vector3& center,
                          const math::Vector3& planeAxisA, const math::Vector3& planeAxisB, float radius)
        {
            for (int segmentIndex = 0; segmentIndex < CIRCLE_SEGMENTS; ++segmentIndex)
            {
                const float startAngle = (static_cast<float>(segmentIndex) / CIRCLE_SEGMENTS) * PI * 2.0f;
                const float endAngle = (static_cast<float>(segmentIndex + 1) / CIRCLE_SEGMENTS) * PI * 2.0f;
                const math::Vector3 from = CirclePoint(center, planeAxisA, planeAxisB, radius, startAngle);
                const math::Vector3 to = CirclePoint(center, planeAxisA, planeAxisB, radius, endAngle);
                AppendLine(geometry, from, to);
            }
        }

        void AppendArc(ColliderDebugGeometry& geometry, const math::Vector3& center,
                       const math::Vector3& planeAxisA, const math::Vector3& planeAxisB,
                       float radius, float arcStart, float arcEnd)
        {
            for (int segmentIndex = 0; segmentIndex < ARC_SEGMENTS; ++segmentIndex)
            {
                const float startFraction = static_cast<float>(segmentIndex) / ARC_SEGMENTS;
                const float endFraction = static_cast<float>(segmentIndex + 1) / ARC_SEGMENTS;
                const float startAngle = arcStart + (arcEnd - arcStart) * startFraction;
                const float endAngle = arcStart + (arcEnd - arcStart) * endFraction;
                const math::Vector3 from = CirclePoint(center, planeAxisA, planeAxisB, radius, startAngle);
                const math::Vector3 to = CirclePoint(center, planeAxisA, planeAxisB, radius, endAngle);
                AppendLine(geometry, from, to);
            }
        }

        AxialBasis BuildAxialBasis(const math::Vector3& shapeAxis)
        {
            const math::Vector3 longitudinalAxis = shapeAxis.Normalized();
            // 主軸が UP に近いときは参照軸を切り替え、正規化前の外積が短くなるのを避ける。
            const math::Vector3 referenceAxis = std::abs(longitudinalAxis.y) > 0.95f
                ? math::Vector3::RIGHT : math::Vector3::UP;
            const math::Vector3 radialAxisA = math::Vector3::Cross(referenceAxis, longitudinalAxis).Normalized();
            const math::Vector3 radialAxisB = math::Vector3::Cross(longitudinalAxis, radialAxisA).Normalized();
            return {radialAxisA, longitudinalAxis, radialAxisB};
        }

        void AppendSideLines(ColliderDebugGeometry& geometry,
                             const math::Vector3& topCenter, const math::Vector3& bottomCenter,
                             const AxialBasis& basis, float radius)
        {
            const math::Vector3 radialOffsetA = basis.radialAxisA * radius;
            const math::Vector3 radialOffsetB = basis.radialAxisB * radius;
            AppendLine(geometry, topCenter + radialOffsetA, bottomCenter + radialOffsetA);
            AppendLine(geometry, topCenter - radialOffsetA, bottomCenter - radialOffsetA);
            AppendLine(geometry, topCenter + radialOffsetB, bottomCenter + radialOffsetB);
            AppendLine(geometry, topCenter - radialOffsetB, bottomCenter - radialOffsetB);
        }
    }

    void AppendBoundsBox(ColliderDebugGeometry& geometry, const math::Vector3& center,
                         const math::Vector3& halfExtents)
    {
        const std::array<math::Vector3, 8> corners = {
            center + math::Vector3{-halfExtents.x, -halfExtents.y, -halfExtents.z},
            center + math::Vector3{ halfExtents.x, -halfExtents.y, -halfExtents.z},
            center + math::Vector3{ halfExtents.x,  halfExtents.y, -halfExtents.z},
            center + math::Vector3{-halfExtents.x,  halfExtents.y, -halfExtents.z},
            center + math::Vector3{-halfExtents.x, -halfExtents.y,  halfExtents.z},
            center + math::Vector3{ halfExtents.x, -halfExtents.y,  halfExtents.z},
            center + math::Vector3{ halfExtents.x,  halfExtents.y,  halfExtents.z},
            center + math::Vector3{-halfExtents.x,  halfExtents.y,  halfExtents.z}
        };
        AppendBoxEdges(geometry, corners);
    }

    ColliderDebugGeometry BuildBoundsGeometry(const Collider& collider)
    {
        ColliderDebugGeometry geometry;
        const AABB bounds = collider.GetAABB();
        AppendBoundsBox(geometry, bounds.Center(), bounds.Extents());
        return geometry;
    }

    ColliderDebugGeometry BuildSphereGeometry(const SphereCollider& sphere)
    {
        ColliderDebugGeometry geometry;
        const math::Vector3 center = sphere.GetAABB().Center();
        AppendCircle(geometry, center, math::Vector3::RIGHT, math::Vector3::UP, sphere.m_radius);
        AppendCircle(geometry, center, math::Vector3::RIGHT, math::Vector3::FORWARD, sphere.m_radius);
        AppendCircle(geometry, center, math::Vector3::UP, math::Vector3::FORWARD, sphere.m_radius);
        return geometry;
    }

    ColliderDebugGeometry BuildOBBGeometry(const OBBCollider& box)
    {
        ColliderDebugGeometry geometry;
        AppendBoxEdges(geometry, box.GetCorners());
        return geometry;
    }

    ColliderDebugGeometry BuildCapsuleGeometry(const CapsuleCollider& capsule)
    {
        const math::Vector3 topCenter = capsule.GetSegmentEnd();
        const math::Vector3 bottomCenter = capsule.GetSegmentStart();
        const math::Vector3 shapeAxis = topCenter - bottomCenter;
        if (shapeAxis.LengthSq() < 1e-6f) {
            return BuildBoundsGeometry(capsule);
        }

        const AxialBasis basis = BuildAxialBasis(shapeAxis);
        const float radius = capsule.m_radius;
        ColliderDebugGeometry geometry;
        AppendCircle(geometry, topCenter, basis.radialAxisA, basis.radialAxisB, radius);
        AppendCircle(geometry, bottomCenter, basis.radialAxisA, basis.radialAxisB, radius);
        AppendArc(geometry, topCenter, basis.radialAxisA, basis.longitudinalAxis, radius, 0.0f, PI);
        AppendArc(geometry, topCenter, basis.radialAxisB, basis.longitudinalAxis, radius, 0.0f, PI);
        AppendArc(geometry, bottomCenter, basis.radialAxisA, basis.longitudinalAxis, radius, PI, PI * 2.0f);
        AppendArc(geometry, bottomCenter, basis.radialAxisB, basis.longitudinalAxis, radius, PI, PI * 2.0f);
        AppendSideLines(geometry, topCenter, bottomCenter, basis, radius);
        return geometry;
    }

    ColliderDebugGeometry BuildCylinderGeometry(const CylinderCollider& cylinder)
    {
        const math::Vector3 topCenter = cylinder.GetSegmentEnd();
        const math::Vector3 bottomCenter = cylinder.GetSegmentStart();
        const math::Vector3 shapeAxis = topCenter - bottomCenter;
        if (shapeAxis.LengthSq() < 1e-6f) {
            return BuildBoundsGeometry(cylinder);
        }

        const AxialBasis basis = BuildAxialBasis(shapeAxis);
        ColliderDebugGeometry geometry;
        AppendCircle(geometry, topCenter, basis.radialAxisA, basis.radialAxisB, cylinder.m_radius);
        AppendCircle(geometry, bottomCenter, basis.radialAxisA, basis.radialAxisB, cylinder.m_radius);
        AppendSideLines(geometry, topCenter, bottomCenter, basis, cylinder.m_radius);
        return geometry;
    }
}
