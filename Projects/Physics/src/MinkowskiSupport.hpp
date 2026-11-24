/// @file    MinkowskiSupport.hpp
/// @brief   凸形状の支持点と Minkowski 差の対応を保持する。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#pragma once
#include <Physics/GJK.hpp>

namespace fbzz::physics::convex_contact_detail
{
    struct MinkowskiSupport
    {
        const void* m_shapeA;
        SupportFn m_supportA;
        const void* m_shapeB;
        SupportFn m_supportB;

        math::Vector3 SampleA(const math::Vector3& direction) const
        {
            return m_supportA(m_shapeA, direction);
        }

        math::Vector3 SampleB(const math::Vector3& direction) const
        {
            return m_supportB(m_shapeB, direction);
        }

        Simplex::Vertex SampleDifference(const math::Vector3& direction) const
        {
            const math::Vector3 supportPointA = SampleA(direction);
            const math::Vector3 supportPointB = SampleB(-direction);
            return {supportPointA - supportPointB, supportPointA, supportPointB};
        }
    };
}
