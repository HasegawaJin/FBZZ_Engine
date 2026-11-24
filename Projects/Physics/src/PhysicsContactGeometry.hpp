/// @file    PhysicsContactGeometry.hpp
/// @brief   プリミティブとメッシュの接触判定で共有する内部関数。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#pragma once

#include <Physics/GJK.hpp>

namespace fbzz::physics
{
    struct ContactPoint;
    struct Triangle;

    namespace contact_geometry_detail
    {
        /// @note ほぼ平行で重なる線分では重なり区間の中央を代表点に選ぶ。
        void ClosestPointsOnSegments(const math::Vector3& startA,
                                     const math::Vector3& endA,
                                     const math::Vector3& startB,
                                     const math::Vector3& endB,
                                     math::Vector3& outClosestA,
                                     math::Vector3& outClosestB);

        bool TestConvexTriangle(const void* shape, SupportFn supportShape,
                                const Triangle& triangle, ContactPoint& outContact);
    }
}
