// FBZZ Engine
// SpringConstraint.cpp | fbzz::physics
// Hooke 則バネ制約
#include <Physics/SpringConstraint.hpp>

namespace fbzz::physics
{
    SpringConstraint::SpringConstraint(RigidBody* bodyA,
                                       RigidBody* bodyB,
                                       float restLength,
                                       float stiffness,
                                       float damping)
        : Constraint(bodyA, bodyB),
          m_restLength(restLength),
          m_stiffness(stiffness),
          m_damping(damping)
    {
    }

    void SpringConstraint::ApplyForce(float /*dt*/)
    {
        if (!m_bodyA || !m_bodyB) return;

        const math::Vector3 delta = m_bodyB->GetPosition() - m_bodyA->GetPosition();
        const float length = delta.Length();
        if (length < 1e-6f) return;

        const math::Vector3 dir = delta * (1.0f / length);
        const float relVel = math::Vector3::Dot(m_bodyB->GetVelocity() - m_bodyA->GetVelocity(), dir);
        const float forceScale = -m_stiffness * (length - m_restLength) - m_damping * relVel;
        const math::Vector3 force = dir * forceScale;

        m_bodyA->ApplyForce(-force);
        m_bodyB->ApplyForce(force);
    }
} // namespace fbzz::physics
