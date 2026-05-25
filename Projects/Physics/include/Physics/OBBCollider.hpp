// FBZZ Engine
// OBBCollider.hpp | fbzz::physics
// Oriented bounding box collider
#pragma once
#include <array>
#include <Physics/Collider.hpp>

namespace fbzz::physics
{
    class OBBCollider : public Collider
    {
    public:
        explicit OBBCollider(const math::Vector3& halfExtents);

        AABB         GetAABB() const override;
        ColliderType GetType() const override { return ColliderType::OBB; }
        void Update(const math::Vector3& worldPos,
                    const math::Quaternion& worldRot) override;

        math::Vector3 GetCenter() const { return m_worldCenter; }
        math::Quaternion GetRotation() const { return m_worldRot; }
        math::Vector3 GetAxis(int index) const;
        std::array<math::Vector3, 8> GetCorners() const;
        math::Vector3 SupportPoint(const math::Vector3& dir) const;

        math::Vector3 m_halfExtents;

    private:
        math::Vector3 m_worldCenter;
        math::Quaternion m_worldRot;
    };

} // namespace fbzz::physics
