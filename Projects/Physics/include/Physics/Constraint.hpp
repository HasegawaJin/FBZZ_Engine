// FBZZ Engine
// Constraint.hpp | fbzz::physics
// 剛体間制約の基底クラス
#pragma once
#include <Physics/RigidBody.hpp>

namespace fbzz::physics
{
    enum class ConstraintType
    {
        DISTANCE,
        SPRING,
        ROPE,
        CHAIN,
        HINGE
    };

    class Constraint
    {
    public:
        Constraint(RigidBody* bodyA, RigidBody* bodyB);
        virtual ~Constraint() = default;

        virtual void ApplyForce(float dt) {}
        virtual void SolvePosition(float dt) {}
        virtual ConstraintType GetType() const = 0;

        RigidBody* GetBodyA() const { return m_bodyA; }
        RigidBody* GetBodyB() const { return m_bodyB; }

    protected:
        RigidBody* m_bodyA = nullptr;
        RigidBody* m_bodyB = nullptr;
    };
} // namespace fbzz::physics
