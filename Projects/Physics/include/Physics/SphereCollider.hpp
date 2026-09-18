/// @file    SphereCollider.hpp
/// @brief   球形コライダー。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <Physics/Collider.hpp>

namespace fbzz::physics {

    /// @brief 回転の影響を受けない球形状。安価で CCD の代表形状としても使う。
    class SphereCollider : public Collider
    {
    public:
        explicit SphereCollider(float radius);

        AABB         GetAABB() const override;
        ColliderType GetType() const override { return ColliderType::SPHERE; }
        void Update(const math::Vector3& worldPos,
                    const math::Quaternion& worldRot) override;
        /// @brief 4/3 π r³。
        [[nodiscard]] float ComputeVolume() const override;

        float m_radius;

    private:
        math::Vector3 m_worldCenter;
    };

} // namespace fbzz::physics
