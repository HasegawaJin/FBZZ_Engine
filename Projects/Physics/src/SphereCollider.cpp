// FBZZ Engine
// SphereCollider.cpp | fbzz::physics
// 球形コライダー
#include <Physics/SphereCollider.hpp>

namespace fbzz::physics
{

    SphereCollider::SphereCollider(float radius)
        : m_radius(radius)
    {}

    float SphereCollider::ComputeVolume() const
    {
        constexpr float PI = 3.14159265358979323846f;
        return (4.0f / 3.0f) * PI * m_radius * m_radius * m_radius;
    }

    AABB SphereCollider::GetAABB() const
    {
        return {
            { m_worldCenter.x - m_radius, m_worldCenter.y - m_radius, m_worldCenter.z - m_radius },
            { m_worldCenter.x + m_radius, m_worldCenter.y + m_radius, m_worldCenter.z + m_radius }
        };
    }

    void SphereCollider::Update(const math::Vector3& worldPos,
                                const math::Quaternion& /*worldRot*/)
    {
        m_worldCenter = worldPos;
    }

} // namespace fbzz::physics
