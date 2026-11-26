/// @file    SpringConstraint.cpp
/// @brief   Hooke 則バネ制約。
/// @author  Hasegawa Jin
/// @date    2026-05-22
#include <Physics/Constraints/SpringConstraint.hpp>

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
        if (!m_bodyA || !m_bodyB) {
            return;
        }

        const math::Vector3 delta = m_bodyB->GetPosition() - m_bodyA->GetPosition();
        const float length = delta.Length();
        if (length < 1e-6f) {
            return;
        }

        const math::Vector3 dir = delta * (1.0f / length);
        // 内積でばねの軸に沿う速度差だけを取り出す。横切る動きには伸び縮みに対する抵抗を掛けない。
        const float relVel = math::Vector3::Dot(m_bodyB->GetVelocity() - m_bodyA->GetVelocity(), dir);
        // 自然長からのずれが復元力を、伸縮する速さが減衰力を決める。どちらもずれや動きを打ち消す向きに働く。
        const float forceScale = -m_stiffness * (length - m_restLength) - m_damping * relVel;
        const math::Vector3 force = dir * forceScale;

        m_bodyA->ApplyForce(-force);
        m_bodyB->ApplyForce(force);
    }
} /// @note namespace fbzz::physics
