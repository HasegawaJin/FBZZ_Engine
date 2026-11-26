/// @file    XPBDGrip.cpp
/// @brief   把持点の陰的ばね減衰を、双方への有限力積として解く。
/// @author  Hasegawa Jin
/// @date    2026-11-03
#include <Physics/Solver/XPBD/XPBDGrip.hpp>
#include <Math/Scalar/MathUtils.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::physics {
namespace {

bool IsFinitePoint(const math::Vector3& point)
{
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

math::Vector3 VelocityAtAnchor(const RigidBody* body, const math::Vector3& anchorOffset)
{
    if (body == nullptr) {
        return math::Vector3::ZERO;
    }
    return body->GetVelocity() + math::Vector3::Cross(body->GetAngularVelocity(), anchorOffset);
}

} // namespace

bool XPBDGrip::Bind(RigidBody* body, RigidBody* other, const math::Vector3& point,
                    const XPBDGripSettings& settings)
{
    // 自分自身への把持や、姿勢が有限でない剛体との把持は作らない。
    if (!body || body == other || !IsFinitePoint(point)) {
        return false;
    }
    if (!body->IsFiniteState() || (other && !other->IsFiniteState())) {
        return false;
    }

    const float limits[] = {settings.frequency, settings.dampingRatio, settings.maxForce, settings.maxTorque,
        settings.breakForce, settings.breakDistance, settings.maxReach};
    for (float limit : limits) {
        if (!std::isfinite(limit) || limit < 0.0f) {
            return false;
        }
    }
    if (settings.frequency <= 0.0f || settings.maxForce <= 0.0f) {
        return false;
    }
    if ((point - body->GetPosition()).Length() > settings.maxReach) {
        return false;
    }

    // 点と姿勢を把持相手の座標系へ移す。相手が静止ワールドならワールド座標のまま持つ。
    m_body = body;
    m_other = other;
    m_settings = settings;
    m_bodyLife = body->ObserveLifetime();
    m_otherLife = other ? other->ObserveLifetime() : std::weak_ptr<const void>{};
    m_localA = body->GetRotation().Inverse() * (point - body->GetPosition());
    m_localB = other ? other->GetRotation().Inverse() * (point - other->GetPosition()) : point;
    m_rotation = other ? (other->GetRotation().Inverse() * body->GetRotation()).Normalized() : body->GetRotation();
    m_status = XPBDGripStatus::HOLDING;
    ResetLambda();
    return true;
}

XPBDGripStatus XPBDGrip::Status() const
{
    return m_status == XPBDGripStatus::HOLDING && !Alive() ? XPBDGripStatus::TARGET_LOST : m_status;
}

math::Vector3 XPBDGrip::Anchor() const
{
    return Alive() ? m_body->GetPosition() + m_body->GetRotation() * m_localA : math::Vector3::ZERO;
}

math::Vector3 XPBDGrip::Target() const
{
    if (!Alive()) {
        return math::Vector3::ZERO;
    }
    if (!m_other) {
        return m_localB;
    }
    return m_other->GetPosition() + m_other->GetRotation() * m_localB;
}

void XPBDGrip::SolvePosition(float h)
{
    if (m_status != XPBDGripStatus::HOLDING || !std::isfinite(h) || h <= 0.0f) {
        return;
    }
    if (!Alive()) {
        m_status = XPBDGripStatus::TARGET_LOST;
        return;
    }

    // 両剛体が把持に応答できるように起こし、アンカー間が離れすぎていれば把持を解除する。
    m_body->WakeUp();
    if (m_other) {
        m_other->WakeUp();
    }
    const math::Vector3 bodyAnchor = Anchor();
    const math::Vector3 targetAnchor = Target();
    const math::Vector3 anchorError = bodyAnchor - targetAnchor;
    if (m_settings.breakDistance > 0.0f && anchorError.Length() > m_settings.breakDistance) {
        m_status = XPBDGripStatus::DISTANCE_LIMIT;
        return;
    }

    // ワールド空間の腕と、並進・回転で共用する後退 Euler の係数を準備する。
    const math::Vector3 bodyAnchorOffset = bodyAnchor - m_body->GetPosition();
    const math::Vector3 targetAnchorOffset = m_other ? targetAnchor - m_other->GetPosition() : math::Vector3::ZERO;
    const float angularFrequency = math::TWO_PI * m_settings.frequency;
    const float damping = 2.0f * m_settings.dampingRatio * angularFrequency;
    const float denominator = 1.0f + damping * h + angularFrequency * angularFrequency * h * h;
    if (!std::isfinite(denominator)) {
        return;
    }

    // 点ばねを予測姿勢へ反映する。相手は再積分せず、速度パスで等量逆向きの力積を返す。
    /// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UPhysicsHandleComponent 把持点と回転保持の分離。
    const math::Vector3 correction = anchorError * (h * angularFrequency * angularFrequency)
        + (VelocityAtAnchor(m_body, bodyAnchorOffset) - VelocityAtAnchor(m_other, targetAnchorOffset)) * (h * damping);
    const math::Vector3 correctionAxis = correction.NormalizedOr(math::Vector3::UP);
    const float inverseMass = GeneralizedInverseMass(m_body, bodyAnchorOffset, correctionAxis)
        + GeneralizedInverseMass(m_other, targetAnchorOffset, correctionAxis);
    if (inverseMass > 0.0f) {
        // 反復中は既に加えた力積の陰的減衰項を残差へ含め、今回の増分だけで合計を更新する。
        const math::Vector3 deltaImpulse = (correction + m_impulse * (inverseMass * (1.0f + h * damping)))
            * (-1.0f / (inverseMass * denominator));
        math::Vector3 totalImpulse = m_impulse + deltaImpulse;
        const float demandedMagnitude = totalImpulse.Length();
        if (m_settings.breakForce > 0.0f && demandedMagnitude / h > m_settings.breakForce) {
            m_status = XPBDGripStatus::FORCE_LIMIT;
            return;
        }
        const float limitedMagnitude = std::min(demandedMagnitude, m_settings.maxForce * h);
        if (demandedMagnitude > 0.0f) {
            totalImpulse = totalImpulse * (limitedMagnitude / demandedMagnitude);
        }
        const math::Vector3 impulse = totalImpulse - m_impulse;
        m_body->SetPosition(m_body->GetPosition() + impulse * (m_body->GetInvMass() * h));
        m_body->SetRotation(IntegrateRotation(m_body->GetRotation(),
            m_body->ApplyInvInertia(math::Vector3::Cross(bodyAnchorOffset, impulse)), h));
        m_impulse = totalImpulse;
        m_reactionPoint = targetAnchor;
        m_force = limitedMagnitude / h;
    }

    // 回転保持を有効にした場合は、把持開始時の相対姿勢からのずれを別の角力積で補う。
    if (m_settings.constrainRotation && m_settings.maxTorque > 0.0f) {
        const math::Quaternion goal = m_other ? (m_other->GetRotation() * m_rotation).Normalized() : m_rotation;
        const math::Vector3 relativeAngularVelocity = m_body->GetAngularVelocity()
            - (m_other ? m_other->GetAngularVelocity() : math::Vector3::ZERO);
        const math::Vector3 angularCorrection = RotationVector((m_body->GetRotation() * goal.Inverse()).Normalized())
            * (h * angularFrequency * angularFrequency) + relativeAngularVelocity * (h * damping);
        const math::Vector3 angularAxis = angularCorrection.NormalizedOr(math::Vector3::UP);
        const float inverseInertia = math::Vector3::Dot(angularAxis, m_body->ApplyInvInertia(angularAxis))
            + (m_other ? math::Vector3::Dot(angularAxis, m_other->ApplyInvInertia(angularAxis)) : 0.0f);
        if (inverseInertia > 0.0f) {
            const math::Vector3 deltaImpulse = (angularCorrection
                + m_angularImpulse * (inverseInertia * (1.0f + h * damping))) * (-1.0f / (inverseInertia * denominator));
            math::Vector3 totalImpulse = m_angularImpulse + deltaImpulse;
            const float magnitude = totalImpulse.Length();
            if (magnitude > m_settings.maxTorque * h) {
                totalImpulse = totalImpulse * (m_settings.maxTorque * h / magnitude);
            }
            const math::Vector3 impulse = totalImpulse - m_angularImpulse;
            m_body->SetRotation(IntegrateRotation(m_body->GetRotation(), m_body->ApplyInvInertia(impulse), h));
            m_angularImpulse = totalImpulse;
        }
    }
}

void XPBDGrip::SolveVelocity(float h)
{
    if (!std::isfinite(h) || h <= 0.0f || m_status != XPBDGripStatus::HOLDING) {
        return;
    }
    if (!Alive()) {
        m_status = XPBDGripStatus::TARGET_LOST;
        return;
    }

    // 位置パスで積んだ反作用を相手に返し、次のサブステップへ持ち越さない。
    if (m_other) {
        m_other->ApplyImpulseAtPoint(-m_impulse, m_reactionPoint);
        m_other->ApplyAngularImpulse(-m_angularImpulse);
    }
    m_impulse = math::Vector3::ZERO;
    m_angularImpulse = math::Vector3::ZERO;
}

} // namespace fbzz::physics
