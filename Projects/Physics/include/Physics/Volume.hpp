/// @file    Volume.hpp
/// @brief   空間効果 Volume の基底クラス。
/// @author  Hasegawa Jin
/// @date    2026-05-22
#pragma once
#include <Physics/RigidBody.hpp>

namespace fbzz::physics
{
    // 範囲内の剛体に継続効果を与える抽象基底。実際の形状判定は派生クラスが担う。
    class Volume
    {
    public:
        virtual ~Volume() = default;

        virtual bool Contains(const math::Vector3& position) const = 0;
        virtual void Apply(RigidBody& body, float dt) = 0;
        virtual float GetTimeScale() const { return 1.0f; }
        virtual bool OverridesGravity() const { return false; }
        virtual bool IsExpired() const { return false; }
        virtual void Tick(float /*dt*/) {}
    };
} // namespace fbzz::physics
