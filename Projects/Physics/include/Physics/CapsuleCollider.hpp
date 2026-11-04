/// @file    CapsuleCollider.hpp
/// @brief   Y 軸向きカプセルコライダー。
/// @author  Hasegawa Jin
/// @date    2026-05-22
#pragma once
#include <Physics/Api.hpp>
#include <Physics/Collider.hpp>

namespace fbzz::physics
{
    /// @note ローカル Y 軸を中心線とするカプセル。回転は Update() でワールド線分へ反映する。
    class CapsuleCollider : public Collider
    {
    public:
        FBZZ_PHYSICS_API CapsuleCollider(float radius, float halfHeight);

        FBZZ_PHYSICS_API AABB         GetAABB() const override;
        ColliderType GetType() const override { return ColliderType::CAPSULE; }
        FBZZ_PHYSICS_API void Update(const math::Vector3& worldPos,
                    const math::Quaternion& worldRot) override;
        /// @note 円柱 (π r² * 2h) + 両端の半球 (= 球 1 個ぶん 4/3 π r³)
        [[nodiscard]] FBZZ_PHYSICS_API float ComputeVolume() const override;

        math::Vector3 GetSegmentStart() const { return m_worldStart; }
        math::Vector3 GetSegmentEnd() const { return m_worldEnd; }

        float m_radius = 0.5f;     ///< @note 半球部分を含む半径
        float m_halfHeight = 1.0f; ///< @note 中心から円柱端までの距離

    private:
        math::Vector3 m_worldCenter;
        math::Vector3 m_worldStart;
        math::Vector3 m_worldEnd;
    };
} /// @note namespace fbzz::physics
