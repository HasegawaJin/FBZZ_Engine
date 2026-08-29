/// @file    FixedConstraint.cpp
/// @brief   2 つの剛体の相対位置・相対回転を固定する溶接拘束。
/// @author  Hasegawa Jin
/// @date    2026-06-08
#include <Physics/FixedConstraint.hpp>

namespace fbzz::physics
{
    FixedConstraint::FixedConstraint(RigidBody* bodyA, RigidBody* bodyB)
        : Constraint(bodyA, bodyB)
    {
        if (!bodyA || !bodyB) return;
        m_localOffsetA = bodyA->GetRotation().Inverse() * (bodyB->GetPosition() - bodyA->GetPosition());
        m_relativeRotation = (bodyA->GetRotation().Inverse() * bodyB->GetRotation()).Normalized();
    }

    void FixedConstraint::SolvePosition(float /*dt*/)
    {
        if (!m_bodyA || !m_bodyB) return;

        const math::Vector3 targetB = m_bodyA->GetPosition() + (m_bodyA->GetRotation() * m_localOffsetA);
        const math::Vector3 delta = m_bodyB->GetPosition() - targetB;
        const float invA = m_bodyA->GetInvMass();
        const float invB = m_bodyB->GetInvMass();
        const float invSum = invA + invB;
        if (invSum == 0.0f) return;

        if (!m_bodyA->IsStatic())
            m_bodyA->SetPosition(m_bodyA->GetPosition() + delta * (invA / invSum));
        if (!m_bodyB->IsStatic())
            m_bodyB->SetPosition(m_bodyB->GetPosition() - delta * (invB / invSum));

        const math::Quaternion targetRotB = (m_bodyA->GetRotation() * m_relativeRotation).Normalized();
        if (m_bodyA->IsStatic() && !m_bodyB->IsStatic()) {
            m_bodyB->SetRotation(targetRotB);
        } else if (!m_bodyA->IsStatic() && m_bodyB->IsStatic()) {
            m_bodyA->SetRotation((m_bodyB->GetRotation() * m_relativeRotation.Inverse()).Normalized());
        } else if (!m_bodyA->IsStatic() && !m_bodyB->IsStatic()) {
            m_bodyB->SetRotation(math::Quaternion::Slerp(m_bodyB->GetRotation(), targetRotB, 0.5f));
            m_bodyA->SetRotation(math::Quaternion::Slerp(
                m_bodyA->GetRotation(),
                (m_bodyB->GetRotation() * m_relativeRotation.Inverse()).Normalized(),
                0.5f));
        }
    }
} // namespace fbzz::physics
