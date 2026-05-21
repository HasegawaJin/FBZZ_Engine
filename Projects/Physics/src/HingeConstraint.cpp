// FBZZ Engine
// HingeConstraint.cpp | fbzz::physics
// ピボット点を共有する簡易ヒンジ制約
#include <Physics/HingeConstraint.hpp>

namespace fbzz::physics
{
    HingeConstraint::HingeConstraint(RigidBody* bodyA,
                                     RigidBody* bodyB,
                                     const math::Vector3& localAnchorA,
                                     const math::Vector3& localAnchorB,
                                     const math::Vector3& axis)
        : Constraint(bodyA, bodyB),
          m_localAnchorA(localAnchorA),
          m_localAnchorB(localAnchorB),
          m_axis(axis.Normalized())
    {
    }

    void HingeConstraint::SolvePosition(float /*dt*/)
    {
        if (!m_bodyA || !m_bodyB) return;

        const math::Vector3 worldAnchorA = m_bodyA->GetPosition() + (m_bodyA->GetRotation() * m_localAnchorA);
        const math::Vector3 worldAnchorB = m_bodyB->GetPosition() + (m_bodyB->GetRotation() * m_localAnchorB);
        const math::Vector3 delta = worldAnchorB - worldAnchorA;

        const float invA = m_bodyA->GetInvMass();
        const float invB = m_bodyB->GetInvMass();
        const float invSum = invA + invB;
        if (invSum == 0.0f) return;

        m_bodyA->SetPosition(m_bodyA->GetPosition() + delta * (invA / invSum));
        m_bodyB->SetPosition(m_bodyB->GetPosition() - delta * (invB / invSum));
    }
} // namespace fbzz::physics
