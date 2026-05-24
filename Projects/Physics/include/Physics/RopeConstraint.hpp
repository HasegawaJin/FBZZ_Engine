// FBZZ Engine
// RopeConstraint.hpp | fbzz::physics
// Maximum distance rope constraint
#pragma once
#include <Physics/Constraint.hpp>

namespace fbzz::physics
{
    class RopeConstraint : public Constraint
    {
    public:
        RopeConstraint(RigidBody* bodyA, RigidBody* bodyB, float maxLength);

        void SolvePosition(float dt) override;
        ConstraintType GetType() const override { return ConstraintType::ROPE; }

        float m_maxLength = 1.0f;
    };
} // namespace fbzz::physics
