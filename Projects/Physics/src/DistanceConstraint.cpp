/// @file    DistanceConstraint.cpp
/// @brief   固定距離を保つ剛体ロッド制約。
/// @author  Hasegawa Jin
/// @date    2026-05-22
#include <Physics/DistanceConstraint.hpp>

namespace fbzz::physics
{
    DistanceConstraint::DistanceConstraint(RigidBody* bodyA, RigidBody* bodyB, float distance)
        : Constraint(bodyA, bodyB), m_distance(distance)
    {
    }

    void DistanceConstraint::SolvePosition(float /*dt*/)
    {
        if (!m_bodyA || !m_bodyB) return;

        const math::Vector3 delta = m_bodyB->GetPosition() - m_bodyA->GetPosition();
        const float length = delta.Length();
        if (length < 1e-6f) return;

        const float invA = m_bodyA->GetInvMass();
        const float invB = m_bodyB->GetInvMass();
        const float invSum = invA + invB;
        if (invSum == 0.0f) return;

        const math::Vector3 correction = delta * ((length - m_distance) / length);
        m_bodyA->SetPosition(m_bodyA->GetPosition() + correction * (invA / invSum));
        m_bodyB->SetPosition(m_bodyB->GetPosition() - correction * (invB / invSum));
    }
} // namespace fbzz::physics
