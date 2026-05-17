// FBZZ Engine
// AABBCollider.hpp | fbzz::physics
// 軸整合バウンディングボックスコライダー
#pragma once
#include <physics/Collider.hpp>

namespace fbzz::physics 
{

    class AABBCollider : public Collider {
    public:
        explicit AABBCollider(const math::Vector3& halfExtents);

        AABB         GetAABB() const override;
        ColliderType GetType() const override { return ColliderType::AABB; }
        void Update(const math::Vector3& worldPos,
                    const math::Quaternion& worldRot) override;

        math::Vector3 m_halfExtents;

    private:
        math::Vector3 m_worldCenter;
    };

} // namespace fbzz::physics
