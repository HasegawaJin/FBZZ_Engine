// FBZZ Engine
// SphereCollider.hpp | fbzz::physics
// 球形コライダー
#pragma once
#include <physics/Collider.hpp>

namespace fbzz::physics {

    class SphereCollider : public Collider 
    {
    public:
        explicit SphereCollider(float radius);

        AABB         GetAABB() const override;
        ColliderType GetType() const override { return ColliderType::SPHERE; }
        void Update(const math::Vector3& worldPos,
                    const math::Quaternion& worldRot) override;

        float m_radius;

    private:
        math::Vector3 m_worldCenter;
    };

} // namespace fbzz::physics
