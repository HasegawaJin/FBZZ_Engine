// FBZZ Engine
// Constraint.hpp | fbzz::physics
// 剛体間制約の基底クラス
#pragma once
#include <Physics/RigidBody.hpp>

namespace fbzz::physics
{
    class Constraint
    {
    public:
        Constraint(RigidBody* bodyA, RigidBody* bodyB);
        virtual ~Constraint() = default;

        virtual void ApplyForce(float dt) {}
        virtual void SolvePosition(float dt) {}

    protected:
        RigidBody* m_bodyA = nullptr;
        RigidBody* m_bodyB = nullptr;
    };
} // namespace fbzz::physics
