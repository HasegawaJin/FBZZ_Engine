// FBZZ Engine
// CapsuleCollider.cpp | fbzz::physics
// Y 軸向きカプセルコライダー
#include <Physics/CapsuleCollider.hpp>

namespace fbzz::physics
{
    CapsuleCollider::CapsuleCollider(float radius, float halfHeight)
        : m_radius(radius), m_halfHeight(halfHeight)
    {
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
