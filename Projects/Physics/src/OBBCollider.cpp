/// @file    OBBCollider.cpp
/// @brief   Oriented bounding box collider.
/// @author  Hasegawa Jin
/// @date    2026-05-25
#include <Physics/OBBCollider.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::physics
{
    OBBCollider::OBBCollider(const math::Vector3& halfExtents)
        : m_halfExtents(halfExtents)
    {
    }

    float OBBCollider::ComputeVolume() const
    {
        return 8.0f * m_halfExtents.x * m_halfExtents.y * m_halfExtents.z;
    }

    AABB OBBCollider::GetAABB() const
    {
        const math::Vector3 x = GetAxis(0);
        const math::Vector3 y = GetAxis(1);
        const math::Vector3 z = GetAxis(2);
        const math::Vector3 extents = {
            std::abs(x.x) * m_halfExtents.x + std::abs(y.x) * m_halfExtents.y + std::abs(z.x) * m_halfExtents.z,
            std::abs(x.y) * m_halfExtents.x + std::abs(y.y) * m_halfExtents.y + std::abs(z.y) * m_halfExtents.z,
            std::abs(x.z) * m_halfExtents.x + std::abs(y.z) * m_halfExtents.y + std::abs(z.z) * m_halfExtents.z
        };

        return { m_worldCenter - extents, m_worldCenter + extents };
    }

    void OBBCollider::Update(const math::Vector3& worldPos,
                             const math::Quaternion& worldRot)
    {
        m_worldCenter = worldPos;
        m_worldRot = worldRot.Normalized();
    }

    math::Vector3 OBBCollider::GetAxis(int index) const
    {
        if (index == 0) return m_worldRot * math::Vector3::RIGHT;
        if (index == 1) return m_worldRot * math::Vector3::UP;
        return m_worldRot * math::Vector3::FORWARD;
    }

    std::array<math::Vector3, 8> OBBCollider::GetCorners() const
    {
        const math::Vector3 x = GetAxis(0) * m_halfExtents.x;
        const math::Vector3 y = GetAxis(1) * m_halfExtents.y;
        const math::Vector3 z = GetAxis(2) * m_halfExtents.z;

        return {
            m_worldCenter - x - y - z,
            m_worldCenter + x - y - z,
            m_worldCenter + x + y - z,
            m_worldCenter - x + y - z,
            m_worldCenter - x - y + z,
            m_worldCenter + x - y + z,
            m_worldCenter + x + y + z,
            m_worldCenter - x + y + z
        };
    }

    math::Vector3 OBBCollider::SupportPoint(const math::Vector3& dir) const
    {
        math::Vector3 result = m_worldCenter;
        for (int i = 0; i < 3; ++i)
        {
            const math::Vector3 axis = GetAxis(i);
            const float extent = i == 0 ? m_halfExtents.x : (i == 1 ? m_halfExtents.y : m_halfExtents.z);
            result += axis * (math::Vector3::Dot(axis, dir) >= 0.0f ? extent : -extent);
        }
        return result;
    }

} // namespace fbzz::physics
