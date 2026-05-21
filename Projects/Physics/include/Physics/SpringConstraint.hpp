// FBZZ Engine
// SpringConstraint.hpp | fbzz::physics
// Hooke spring constraint
#pragma once
#include <Physics/Constraint.hpp>

namespace fbzz::physics
{
    class SpringConstraint : public Constraint
    {
    public:
        SpringConstraint(RigidBody* bodyA,
                         RigidBody* bodyB,
                         float restLength,
                         float stiffness,
                         float damping);

        void ApplyForce(float dt) override;

        float m_restLength = 1.0f;
        float m_stiffness = 10.0f;
        float m_damping = 0.5f;
    };
} // namespace fbzz::physics
