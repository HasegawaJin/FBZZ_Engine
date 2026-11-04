/// @file    XPBDGrip.cpp
/// @brief   把持点の陰的ばね減衰を、双方への有限力積として解く。
/// @author  Hasegawa Jin
/// @date    2026-11-03
#include <Physics/XPBDGrip.hpp>
#include <Math/MathUtils.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::physics {
namespace {
bool Finite(const math::Vector3& p) {return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
math::Vector3 Velocity(const RigidBody* body,const math::Vector3& arm)
{return body ? body->GetVelocity()+math::Vector3::Cross(body->GetAngularVelocity(),arm) : math::Vector3::ZERO;}
}
bool XPBDGrip::Bind(RigidBody* body,RigidBody* other,const math::Vector3& point,const XPBDGripSettings& settings)
{
    if (!body || body==other || !Finite(point) || !body->IsFiniteState() || (other&&!other->IsFiniteState())) return false;
    const float fields[]={settings.frequency,settings.dampingRatio,settings.maxForce,settings.maxTorque,
        settings.breakForce,settings.breakDistance,settings.maxReach};
    for (float f:fields) if (!std::isfinite(f)||f<0.0f) return false;
    if (settings.frequency<=0.0f || settings.maxForce<=0.0f
        || (point-body->GetPosition()).Length()>settings.maxReach) return false;
    m_body=body; m_other=other; m_settings=settings;
    m_bodyLife=body->ObserveLifetime(); m_otherLife=other ? other->ObserveLifetime() : std::weak_ptr<const void>{};
    m_localA=body->GetRotation().Inverse()*(point-body->GetPosition());
    m_localB=other ? other->GetRotation().Inverse()*(point-other->GetPosition()) : point;
    m_rotation=other ? (other->GetRotation().Inverse()*body->GetRotation()).Normalized() : body->GetRotation();
    m_status=XPBDGripStatus::HOLDING; ResetLambda();
    return true;
}
XPBDGripStatus XPBDGrip::Status() const
{return m_status==XPBDGripStatus::HOLDING && !Alive() ? XPBDGripStatus::TARGET_LOST : m_status;}
math::Vector3 XPBDGrip::Anchor() const
{return Alive() ? m_body->GetPosition()+m_body->GetRotation()*m_localA : math::Vector3::ZERO;}
math::Vector3 XPBDGrip::Target() const
{return Alive() ? (m_other ? m_other->GetPosition()+m_other->GetRotation()*m_localB : m_localB) : math::Vector3::ZERO;}

void XPBDGrip::SolvePosition(float h)
{
    if (m_status!=XPBDGripStatus::HOLDING || !std::isfinite(h) || h<=0.0f) return;
    if (!Alive()) {m_status=XPBDGripStatus::TARGET_LOST; return;}
    m_body->WakeUp();if(m_other)m_other->WakeUp();
    const auto a=Anchor(),b=Target(),error=a-b;
    if (m_settings.breakDistance>0.0f && error.Length()>m_settings.breakDistance) {
        m_status=XPBDGripStatus::DISTANCE_LIMIT; return;
    }
    const auto rA=a-m_body->GetPosition(),rB=m_other ? b-m_other->GetPosition() : math::Vector3::ZERO;
    const float omega=math::TWO_PI*m_settings.frequency;
    const float damping=2.0f*m_settings.dampingRatio*omega;
    const float denominator=1.0f+damping*h+omega*omega*h*h;
    if(!std::isfinite(denominator))return;
    /// @note 後退Eulerの点ばねを予測姿勢へ反映する。相手は再積分せず、速度パスで等量逆向きの力積だけを返す。
    /// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UPhysicsHandleComponent 把持点と回転保持の分離。
    const auto correction=error*(h*omega*omega)+(Velocity(m_body,rA)-Velocity(m_other,rB))*(h*damping);
    const auto axis=correction.NormalizedOr(math::Vector3::UP);
    const float w=GeneralizedInverseMass(m_body,rA,axis)+GeneralizedInverseMass(m_other,rB,axis);
    if (w>0.0f) {
        /// @note 反復中は既に加えた力積の陰的減衰項を残差から引き、合計の力積を上限内へ保つ。
        const auto delta=(correction+m_impulse*(w*(1.0f+h*damping)))*(-1.0f/(w*denominator));
        auto total=m_impulse+delta;
        const float demand=total.Length();
        if (m_settings.breakForce>0.0f && demand/h>m_settings.breakForce) {
            m_status=XPBDGripStatus::FORCE_LIMIT; return;
        }
        const float magnitude=std::min(demand,m_settings.maxForce*h);
        if (demand>0.0f) total=total*(magnitude/demand);
        const auto impulse=total-m_impulse;
        m_body->SetPosition(m_body->GetPosition()+impulse*(m_body->GetInvMass()*h));
        m_body->SetRotation(IntegrateRotation(m_body->GetRotation(),m_body->ApplyInvInertia(math::Vector3::Cross(rA,impulse)),h));
        m_impulse=total;m_reactionPoint=b;
        m_force=magnitude/h;
    }
    if (m_settings.constrainRotation && m_settings.maxTorque>0.0f) {
        const auto goal=m_other ? (m_other->GetRotation()*m_rotation).Normalized() : m_rotation;
        const auto angular=m_body->GetAngularVelocity()-(m_other ? m_other->GetAngularVelocity() : math::Vector3::ZERO);
        const auto c=RotationVector((m_body->GetRotation()*goal.Inverse()).Normalized())*(h*omega*omega)+angular*(h*damping);
        const auto n=c.NormalizedOr(math::Vector3::UP);
        const float inv=math::Vector3::Dot(n,m_body->ApplyInvInertia(n))
            +(m_other ? math::Vector3::Dot(n,m_other->ApplyInvInertia(n)) : 0.0f);
        if (inv>0.0f) {
            const auto delta=(c+m_angularImpulse*(inv*(1.0f+h*damping)))*(-1.0f/(inv*denominator));
            auto total=m_angularImpulse+delta;
            const float magnitude=total.Length();
            if (magnitude>m_settings.maxTorque*h) total=total*(m_settings.maxTorque*h/magnitude);
            const auto impulse=total-m_angularImpulse;
            m_body->SetRotation(IntegrateRotation(m_body->GetRotation(),m_body->ApplyInvInertia(impulse),h));
            m_angularImpulse=total;
        }
    }
}
void XPBDGrip::SolveVelocity(float h)
{
    if(!std::isfinite(h)||h<=0||m_status!=XPBDGripStatus::HOLDING)return;
    if(!Alive()){m_status=XPBDGripStatus::TARGET_LOST;return;}
    if(m_other){m_other->ApplyImpulseAtPoint(-m_impulse,m_reactionPoint);m_other->ApplyAngularImpulse(-m_angularImpulse);}
    m_impulse=math::Vector3::ZERO;m_angularImpulse=math::Vector3::ZERO;
}
}
