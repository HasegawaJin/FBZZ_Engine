/// @file    XPBDBodyDrive.cpp
/// @brief   陰的ばね減衰を XPBD の位置・角度補正として解く。
/// @author  Hasegawa Jin
/// @date    2026-11-03
#include <Physics/XPBDBodyDrive.hpp>
#include <Math/MathUtils.hpp>
#include <cmath>

namespace fbzz::physics {
void XPBDBodyDrive::SolvePosition(float h)
{
    if (!m_settings.enabled || !m_body || h<=0.0f) return;
    const auto reference=m_parent ? m_parent->GetRotation() : math::Quaternion::Identity();
    const auto r=m_parent ? reference*m_targetPosition : math::Vector3::ZERO;
    const auto goal=m_parent ? m_parent->GetPosition()+r : m_targetPosition;
    const auto goalRotation=(reference*m_targetRotation).Normalized();
    const auto parentVelocity=m_parent ? m_parent->GetVelocity()+math::Vector3::Cross(m_parent->GetAngularVelocity(),r) : math::Vector3::ZERO;
    const auto parentAngular=m_parent ? m_parent->GetAngularVelocity() : math::Vector3::ZERO;
    const float multiplier=m_settings.targetVelocityMultiplier;
    /// @note 後退 Euler: k=ω²/w, c=2ζω/w。Ceff=C+(c/k)·vError、αeff=(1+h·c·w)/k。
    /// @note λ/h² が力となる同一パスでばねと減衰を制限し、速度パスで出力上限を二重に消費しない。
    /// @see https://matthias-research.github.io/pages/publications/PBDBodies.pdf §3 XPBD の一般化逆質量と力の関係。
    if (m_settings.linearFrequency>0.0f && m_settings.maxForce>0.0f) {
        const float omega=math::TWO_PI*m_settings.linearFrequency;
        const float damping=2.0f*m_settings.linearDampingRatio/omega;
        const auto velocity=m_body->GetVelocity()-parentVelocity-reference*m_targetVelocity*multiplier;
        const auto correction=m_body->GetPosition()-goal+velocity*damping;
        const auto axis=correction.NormalizedOr(math::Vector3::UP);
        const float w=GeneralizedInverseMass(m_body,math::Vector3::ZERO,axis)+GeneralizedInverseMass(m_parent,r,axis);
        if (w>0.0f) SolvePositional(m_body,m_parent,math::Vector3::ZERO,r,correction,
            w*(1.0f/(omega*omega)+damping*h),h,m_linearLambda,m_settings.maxForce*h*h);
        m_linearSaturated=std::abs(m_linearLambda)>=m_settings.maxForce*h*h*0.999f;
    }
    if (m_settings.angularFrequency>0.0f && m_settings.maxTorque>0.0f) {
        const float omega=math::TWO_PI*m_settings.angularFrequency;
        const float damping=2.0f*m_settings.angularDampingRatio/omega;
        const auto velocity=m_body->GetAngularVelocity()-parentAngular-reference*m_targetAngular*multiplier;
        const auto correction=RotationVector((m_body->GetRotation()*goalRotation.Inverse()).Normalized())+velocity*damping;
        const auto axis=correction.NormalizedOr(math::Vector3::UP);
        const float w=math::Vector3::Dot(axis,m_body->ApplyInvInertia(axis))
            +(m_parent ? math::Vector3::Dot(axis,m_parent->ApplyInvInertia(axis)) : 0.0f);
        if (w>0.0f) SolveAngular(m_body,m_parent,correction,w*(1.0f/(omega*omega)+damping*h),
            h,m_angularLambda,m_settings.maxTorque*h*h);
        m_angularSaturated=std::abs(m_angularLambda)>=m_settings.maxTorque*h*h*0.999f;
    }
}
}
