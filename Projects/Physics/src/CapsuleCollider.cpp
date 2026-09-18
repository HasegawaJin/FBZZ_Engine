/// @file    CapsuleCollider.cpp
/// @brief   Y 軸向きカプセルコライダー。
/// @author  Hasegawa Jin
/// @date    2026-05-22
#include <Physics/CapsuleCollider.hpp>

namespace fbzz::physics
{
    CapsuleCollider::CapsuleCollider(float radius, float halfHeight)
        : m_radius(radius), m_halfHeight(halfHeight)
    {
    }

    float CapsuleCollider::ComputeVolume() const
    {
        constexpr float PI = 3.14159265358979323846f;
        const float radiusSq = m_radius * m_radius;
        /// @note m_halfHeight は中心から円柱端までの距離なので、円柱の全長は 2 * halfHeight。
        const float cylinder = PI * radiusSq * (2.0f * m_halfHeight);
        /// @note 両端の半球を合わせると球 1 個ぶん。
        const float hemispheres = (4.0f / 3.0f) * PI * radiusSq * m_radius;
        return cylinder + hemispheres;
    }

    AABB CapsuleCollider::GetAABB() const
    {
        const math::Vector3 radiusExtents{ m_radius, m_radius, m_radius };
        const math::Vector3 minPoint = {
            m_worldStart.x < m_worldEnd.x ? m_worldStart.x : m_worldEnd.x,
            m_worldStart.y < m_worldEnd.y ? m_worldStart.y : m_worldEnd.y,
            m_worldStart.z < m_worldEnd.z ? m_worldStart.z : m_worldEnd.z
        };
        const math::Vector3 maxPoint = {
            m_worldStart.x > m_worldEnd.x ? m_worldStart.x : m_worldEnd.x,
            m_worldStart.y > m_worldEnd.y ? m_worldStart.y : m_worldEnd.y,
            m_worldStart.z > m_worldEnd.z ? m_worldStart.z : m_worldEnd.z
        };
        return { minPoint - radiusExtents, maxPoint + radiusExtents };
    }

    void CapsuleCollider::Update(const math::Vector3& worldPos,
                                 const math::Quaternion& worldRot)
    {
        m_worldCenter = worldPos;
        const math::Vector3 axis = worldRot * math::Vector3::UP;
        m_worldStart = worldPos - axis * m_halfHeight;
        m_worldEnd = worldPos + axis * m_halfHeight;
    }
} // namespace fbzz::physics
