// FBZZ Engine
// SliderConstraint.cpp | fbzz::physics
// 1 軸方向の移動だけを許すプリズマティック拘束
#include <Physics/SliderConstraint.hpp>
#include <algorithm>

namespace fbzz::physics
{
    SliderConstraint::SliderConstraint(RigidBody* bodyA,
                                       RigidBody* bodyB,
                                       const math::Vector3& axis)
        : Constraint(bodyA, bodyB)
        , m_axis(axis.Normalized())
    {
    }

    void SliderConstraint::SetLimits(float minDistance, float maxDistance)
    {
        m_limitEnabled = true;
        m_minDistance = std::min(minDistance, maxDistance);
        m_maxDistance = std::max(minDistance, maxDistance);
    }

    void SliderConstraint::ClearLimits()
    {
        m_limitEnabled = false;
    }

    void SliderConstraint::SolvePosition(float /*dt*/)
    {
        if (!m_bodyA || !m_bodyB) return;

        const math::Vector3 axis = m_axis.Normalized();
        const math::Vector3 currentOffset = m_bodyB->GetPosition() - m_bodyA->GetPosition();
        float distance = math::Vector3::Dot(currentOffset, axis);
        if (m_limitEnabled)
            distance = std::clamp(distance, m_minDistance, m_maxDistance);

        const math::Vector3 targetOffset = axis * distance;
        const math::Vector3 error = currentOffset - targetOffset;
        const float invA = m_bodyA->GetInvMass();
        const float invB = m_bodyB->GetInvMass();
        const float invSum = invA + invB;
        if (invSum == 0.0f) return;

        if (!m_bodyA->IsStatic())
            m_bodyA->SetPosition(m_bodyA->GetPosition() + error * (invA / invSum));
        if (!m_bodyB->IsStatic())
            m_bodyB->SetPosition(m_bodyB->GetPosition() - error * (invB / invSum));
    }
} // namespace fbzz::physics
