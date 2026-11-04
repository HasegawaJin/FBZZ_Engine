/// @file    OBBCollider.hpp
/// @brief   Oriented bounding box collider.
/// @author  Hasegawa Jin
/// @date    2026-05-25
#pragma once
#include <Physics/Api.hpp>
#include <array>
#include <Physics/Collider.hpp>

namespace fbzz::physics
{
    class OBBCollider : public Collider
    {
    public:
        FBZZ_PHYSICS_API explicit OBBCollider(const math::Vector3& halfExtents);

        FBZZ_PHYSICS_API AABB         GetAABB() const override;
        ColliderType GetType() const override { return ColliderType::OBB; }
        FBZZ_PHYSICS_API void Update(const math::Vector3& worldPos,
                    const math::Quaternion& worldRot) override;
        /// @brief 8 * hx * hy * hz。回転しても体積は変わらないため AABB 版と同じ式。
        [[nodiscard]] FBZZ_PHYSICS_API float ComputeVolume() const override;

        math::Vector3 GetCenter() const { return m_worldCenter; }
        math::Quaternion GetRotation() const { return m_worldRot; }
        FBZZ_PHYSICS_API math::Vector3 GetAxis(int index) const;
        FBZZ_PHYSICS_API std::array<math::Vector3, 8> GetCorners() const;
        FBZZ_PHYSICS_API math::Vector3 SupportPoint(const math::Vector3& dir) const;

        math::Vector3 m_halfExtents;

    private:
        math::Vector3 m_worldCenter;
        math::Quaternion m_worldRot;
    };

} /// @note namespace fbzz::physics
