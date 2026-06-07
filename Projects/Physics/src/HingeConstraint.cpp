// FBZZ Engine
// HingeConstraint.cpp | fbzz::physics
// ピボット点を共有する簡易ヒンジ制約
#include <Physics/HingeConstraint.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::physics
{
    namespace
    {
        math::Vector3 AnyPerpendicular(const math::Vector3& axis)
        {
            const math::Vector3 base = std::abs(axis.y) < 0.9f
                ? math::Vector3::UP
                : math::Vector3::RIGHT;
            return math::Vector3::Cross(axis, base).Normalized();
        }

        math::Vector3 ProjectPerpendicular(const math::Vector3& v, const math::Vector3& axis)
        {
            const math::Vector3 projected = v - axis * math::Vector3::Dot(v, axis);
            return projected.LengthSq() > 1e-8f ? projected.Normalized() : AnyPerpendicular(axis);
        }
    }

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
        const math::Vector3 reference = AnyPerpendicular(m_axis);
        m_referenceA = bodyA ? bodyA->GetRotation().Inverse() * reference : reference;
        m_referenceB = bodyB ? bodyB->GetRotation().Inverse() * reference : reference;
    }

    void HingeConstraint::SetLimits(float minAngleRad, float maxAngleRad)
    {
        m_limitEnabled = true;
        m_minAngle = std::min(minAngleRad, maxAngleRad);
        m_maxAngle = std::max(minAngleRad, maxAngleRad);
    }

    void HingeConstraint::ClearLimits()
    {
        m_limitEnabled = false;
    }

    void HingeConstraint::SetMotor(float targetAngularSpeed, float maxTorque)
    {
        m_motorEnabled = true;
        m_motorTargetSpeed = targetAngularSpeed;
        m_motorMaxTorque = std::max(maxTorque, 0.0f);
    }

    void HingeConstraint::ClearMotor()
    {
        m_motorEnabled = false;
    }

    float HingeConstraint::CurrentAngle() const
    {
        if (!m_bodyA || !m_bodyB) return 0.0f;

        const math::Vector3 axis = m_axis.Normalized();
        const math::Vector3 refA = ProjectPerpendicular(m_bodyA->GetRotation() * m_referenceA, axis);
        const math::Vector3 refB = ProjectPerpendicular(m_bodyB->GetRotation() * m_referenceB, axis);
        const float sinAngle = math::Vector3::Dot(axis, math::Vector3::Cross(refA, refB));
        const float cosAngle = std::clamp(math::Vector3::Dot(refA, refB), -1.0f, 1.0f);
        return std::atan2(sinAngle, cosAngle);
    }

    void HingeConstraint::ApplyForce(float dt)
    {
        if (!m_motorEnabled || !m_bodyA || !m_bodyB || dt <= 0.0f) return;

        const math::Vector3 axis = m_axis.Normalized();
        const float relSpeed = math::Vector3::Dot(
            m_bodyB->GetAngularVelocity() - m_bodyA->GetAngularVelocity(), axis);
        const float speedError = m_motorTargetSpeed - relSpeed;
        const float torqueMag = std::clamp(speedError / dt, -m_motorMaxTorque, m_motorMaxTorque);
        const math::Vector3 torque = axis * torqueMag;

        if (!m_bodyA->IsStatic()) m_bodyA->ApplyTorque(-torque);
        if (!m_bodyB->IsStatic()) m_bodyB->ApplyTorque(torque);
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

        if (!m_limitEnabled) return;

        const float angle = CurrentAngle();
        const float clampedAngle = std::clamp(angle, m_minAngle, m_maxAngle);
        const float error = angle - clampedAngle;
        if (std::abs(error) < 1e-4f) return;

        const float weightA = invA / invSum;
        const float weightB = invB / invSum;
        const math::Quaternion corrA = math::Quaternion::FromAxisAngle(m_axis, error * weightA);
        const math::Quaternion corrB = math::Quaternion::FromAxisAngle(m_axis, -error * weightB);
        if (!m_bodyA->IsStatic())
            m_bodyA->SetRotation((corrA * m_bodyA->GetRotation()).Normalized());
        if (!m_bodyB->IsStatic())
            m_bodyB->SetRotation((corrB * m_bodyB->GetRotation()).Normalized());
    }
} // namespace fbzz::physics
