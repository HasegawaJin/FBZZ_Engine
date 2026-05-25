// FBZZ Engine
// AABBCollider.hpp | fbzz::physics
// 軸整合バウンディングボックスコライダー
#pragma once
#include <Physics/Collider.hpp>

namespace fbzz::physics 
{

    // 回転しない箱形状。地面や壁のようにワールド軸へ固定したい衝突に使う。
    class AABBCollider : public Collider {
    public:
        explicit AABBCollider(const math::Vector3& halfExtents);

        AABB         GetAABB() const override;
        ColliderType GetType() const override { return ColliderType::AABB; }
        void Update(const math::Vector3& worldPos,
                    const math::Quaternion& worldRot) override;

        // 中心から各面までの距離。Update() 後もローカル軸ではなくワールド軸基準で扱う。
        math::Vector3 m_halfExtents;

    private:
        math::Vector3 m_worldCenter;
    };

} // namespace fbzz::physics
