// FBZZ Engine
// ChainConstraint.hpp | fbzz::physics
// Multi-body fixed segment chain constraint
#pragma once
#include <Physics/Constraint.hpp>
#include <vector>

namespace fbzz::physics
{
    class ChainConstraint : public Constraint
    {
    public:
        ChainConstraint(std::vector<RigidBody*> bodies, float segmentLength, int solverIterations = 4);

        void ApplyForce(float /*dt*/) override {}
        void SolvePosition(float dt) override;

    private:
        std::vector<RigidBody*> m_bodies;
        float m_segmentLength = 1.0f;
        int m_solverIterations = 4;
    };
} // namespace fbzz::physics
