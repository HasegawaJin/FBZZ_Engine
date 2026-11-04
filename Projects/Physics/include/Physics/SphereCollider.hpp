/// @file    SphereCollider.hpp
/// @brief   球形コライダー。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <Physics/Api.hpp>
#include <Physics/Collider.hpp>

namespace fbzz::physics {

    /// @brief 回転の影響を受けない球形状。安価で CCD の代表形状としても使う。
    class SphereCollider : public Collider
    {
    public:
        FBZZ_PHYSICS_API explicit SphereCollider(float radius);

        FBZZ_PHYSICS_API AABB         GetAABB() const override;
        ColliderType GetType() const override { return ColliderType::SPHERE; }
        FBZZ_PHYSICS_API void Update(const math::Vector3& worldPos,
                    const math::Quaternion& worldRot) override;
        /// @brief 4/3 π r³。
        [[nodiscard]] FBZZ_PHYSICS_API float ComputeVolume() const override;

        float m_radius;

    private:
        math::Vector3 m_worldCenter;
    };

} /// @note namespace fbzz::physics
