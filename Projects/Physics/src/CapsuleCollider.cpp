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
        const math::Vector3 extents = {
            m_radius,
            m_halfHeight + m_radius,
            m_radius
        };
        return { m_worldCenter - extents, m_worldCenter + extents };
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
