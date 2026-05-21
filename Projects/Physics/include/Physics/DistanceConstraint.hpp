// FBZZ Engine
// DistanceConstraint.hpp | fbzz::physics
// Fixed distance constraint
#pragma once
#include <Physics/Constraint.hpp>

namespace fbzz::physics
{
    class DistanceConstraint : public Constraint
    {
    public:
        DistanceConstraint(RigidBody* bodyA, RigidBody* bodyB, float distance);

        void SolvePosition(float dt) override;

        float m_distance = 1.0f;
    };
} // namespace fbzz::physics
