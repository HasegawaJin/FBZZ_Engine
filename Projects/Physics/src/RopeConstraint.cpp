/// @file    RopeConstraint.cpp
/// @brief   最大距離のみを拘束するロープ制約。
/// @author  Hasegawa Jin
/// @date    2026-05-22
#include <Physics/RopeConstraint.hpp>

namespace fbzz::physics
{
    RopeConstraint::RopeConstraint(RigidBody* bodyA, RigidBody* bodyB, float maxLength)
        : Constraint(bodyA, bodyB), m_maxLength(maxLength)
    {
    }

    void RopeConstraint::SolvePosition(float /*dt*/)
    {
        if (!m_bodyA || !m_bodyB) return;

        const math::Vector3 delta = m_bodyB->GetPosition() - m_bodyA->GetPosition();
        const float length = delta.Length();
        /// @note ロープは張ったときだけ両端を引く。たるんだ間も距離を補正すると、ロープが棒のように両端を押してしまう。
        if (length <= m_maxLength || length < 1e-6f) return;

        const float invA = m_bodyA->GetInvMass();
        const float invB = m_bodyB->GetInvMass();
        const float invSum = invA + invB;
        if (invSum == 0.0f) return;

        const math::Vector3 correction = delta * ((length - m_maxLength) / length);
        m_bodyA->SetPosition(m_bodyA->GetPosition() + correction * (invA / invSum));
        m_bodyB->SetPosition(m_bodyB->GetPosition() - correction * (invB / invSum));
    }
} /// @note namespace fbzz::physics
