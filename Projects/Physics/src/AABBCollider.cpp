// FBZZ Engine
// AABBCollider.cpp | fbzz::physics
// 軸整合バウンディングボックスコライダー
#include <Physics/AABBCollider.hpp>

namespace fbzz::physics 
{

    AABBCollider::AABBCollider(const math::Vector3& halfExtents)
        : m_halfExtents(halfExtents)
    {}

    float AABBCollider::ComputeVolume() const
    {
        return 8.0f * m_halfExtents.x * m_halfExtents.y * m_halfExtents.z;
    }

    AABB AABBCollider::GetAABB() const
    {
        return { m_worldCenter - m_halfExtents, m_worldCenter + m_halfExtents };
    }

    void AABBCollider::Update(const math::Vector3& worldPos,
                            const math::Quaternion& /*worldRot*/)
    {
        // AABB は軸整合のため回転は無視し、重心位置のみ追随する
        m_worldCenter = worldPos;
    }

} // namespace fbzz::physics
