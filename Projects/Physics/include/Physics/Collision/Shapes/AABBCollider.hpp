/// @file    AABBCollider.hpp
/// @brief   軸整合バウンディングボックスコライダー。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <Physics/Api.hpp>
#include <Physics/Collision/Shapes/Collider.hpp>

namespace fbzz::physics 
{

    /// @note 回転しない箱形状。地面や壁のようにワールド軸へ固定したい衝突に使う。
    class AABBCollider : public Collider {
    public:
        FBZZ_PHYSICS_API explicit AABBCollider(const math::Vector3& halfExtents);

        FBZZ_PHYSICS_API AABB         GetAABB() const override;
        ColliderType GetType() const override { return ColliderType::AABB; }
        FBZZ_PHYSICS_API void Update(const math::Vector3& worldPos,
                    const math::Quaternion& worldRot) override;
        /// @note 8 * hx * hy * hz
        [[nodiscard]] FBZZ_PHYSICS_API float ComputeVolume() const override;

        math::Vector3 m_halfExtents; ///< 中心から各面までの距離。Update() 後もローカル軸ではなくワールド軸基準で扱う。

    private:
        math::Vector3 m_worldCenter; ///< ワールド空間に変換済みのコライダーの中心 [m]。
    };

} /// @note namespace fbzz::physics
