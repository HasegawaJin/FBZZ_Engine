// FBZZ Engine
// Volume.hpp | fbzz::physics
// 空間効果 Volume の基底クラス
#pragma once
#include <Physics/RigidBody.hpp>

namespace fbzz::physics
{
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
