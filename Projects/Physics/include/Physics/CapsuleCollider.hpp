// FBZZ Engine
// CapsuleCollider.hpp | fbzz::physics
// Y 軸向きカプセルコライダー
#pragma once
#include <Physics/Collider.hpp>

namespace fbzz::physics
{
    // ローカル Y 軸を中心線とするカプセル。回転は Update() でワールド線分へ反映する。
    class CapsuleCollider : public Collider
    {
    public:
        CapsuleCollider(float radius, float halfHeight);

        AABB         GetAABB() const override;
        ColliderType GetType() const override { return ColliderType::CAPSULE; }
        void Update(const math::Vector3& worldPos,
                    const math::Quaternion& worldRot) override;

        math::Vector3 GetSegmentStart() const { return m_worldStart; }
        math::Vector3 GetSegmentEnd() const { return m_worldEnd; }

        float m_radius = 0.5f;     // 半球部分を含む半径
        float m_halfHeight = 1.0f; // 中心から円柱端までの距離

    private:
        math::Vector3 m_worldCenter;
        math::Vector3 m_worldStart;
        math::Vector3 m_worldEnd;
    };
} // namespace fbzz::physics
