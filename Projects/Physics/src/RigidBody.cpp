// FBZZ Engine
// RigidBody.cpp | fbzz::physics
// 剛体の状態と力の積分
#include <Physics/RigidBody.hpp>
#include <Physics/SphereCollider.hpp>
#include <Physics/AABBCollider.hpp>
#include <Physics/CapsuleCollider.hpp>

namespace fbzz::physics {

void RigidBody::ApplyForce(const math::Vector3& force)
{
    m_force += force;
}

void RigidBody::ApplyForceAtPoint(const math::Vector3& force,
                                   const math::Vector3& worldPoint)
{
    m_force  += force;
    m_torque += math::Vector3::Cross(worldPoint - m_position, force);
}

void RigidBody::ApplyImpulse(const math::Vector3& impulse)
{
    m_velocity += impulse * m_invMass;
}

void RigidBody::ApplyAngularImpulse(const math::Vector3& angularImpulse)
{
    m_angularVelocity += ApplyInvInertia(angularImpulse);
}

void RigidBody::ApplyTorque(const math::Vector3& torque)
{
    m_torque += torque;
}

void RigidBody::SetMass(float mass)
{
    m_mass    = mass;
    m_invMass = (m_isStatic || mass == 0.0f) ? 0.0f : 1.0f / mass;
    RecomputeInertia();
}

void RigidBody::SetPosition(const math::Vector3& pos)    { m_position        = pos;    }
void RigidBody::SetVelocity(const math::Vector3& vel)    { m_velocity        = vel;    }
void RigidBody::SetAngularVelocity(const math::Vector3& v) { m_angularVelocity = v;    }

void RigidBody::SetRotation(const math::Quaternion& rot)
{
    m_rotation = rot.Normalized();
}

void RigidBody::SetInertiaFromCollider(const Collider* collider)
{
    m_inertiaCollider = collider;
    RecomputeInertia();
}

void RigidBody::Integrate(float dt)
{
    if (!m_isStatic)
    {
        // 線形運動は半陰的オイラーで積分する。速度を先に更新するため単純な陽的オイラーより安定する。
        // m_force には World が事前に重力・Volume・制約力を ApplyForce 済み。
        m_velocity += m_force * m_invMass * dt;
        m_position += m_velocity * dt;

        // 角運動はボディ空間の対角慣性テンソルで角加速度を求め、ワールド空間へ戻す。
        math::Vector3 tauBody   = m_rotation.Conjugate() * m_torque;
        math::Vector3 alphaBody = {
            m_invInertiaDiag.x * tauBody.x,
            m_invInertiaDiag.y * tauBody.y,
            m_invInertiaDiag.z * tauBody.z
        };
        m_angularVelocity += (m_rotation * alphaBody) * dt;

        // q_dot = 0.5 * [0, ω] * q
        math::Quaternion omegaQuat(
            m_angularVelocity.x, m_angularVelocity.y, m_angularVelocity.z, 0.0f);
        math::Quaternion spin = omegaQuat * m_rotation;
        m_rotation = math::Quaternion(
            m_rotation.x + spin.x * 0.5f * dt,
            m_rotation.y + spin.y * 0.5f * dt,
            m_rotation.z + spin.z * 0.5f * dt,
            m_rotation.w + spin.w * 0.5f * dt
        ).Normalized();
    }

    // static ボディ含め毎フレームリセットする。力は「そのフレームだけ有効」な入力として扱う。
    m_force  = math::Vector3::ZERO;
    m_torque = math::Vector3::ZERO;
}

math::Vector3 RigidBody::ApplyInvInertia(const math::Vector3& v) const
{
    if (m_isStatic) return math::Vector3::ZERO;
    // I⁻¹ v = R * (invInertiaDiag ⊙ (Rᵀ * v))
    math::Vector3 local = m_rotation.Conjugate() * v;
    math::Vector3 scaled = {
        m_invInertiaDiag.x * local.x,
        m_invInertiaDiag.y * local.y,
        m_invInertiaDiag.z * local.z
    };
    return m_rotation * scaled;
}

void RigidBody::RecomputeInertia()
{
    if (m_isStatic)
    {
        m_invInertiaDiag = math::Vector3::ZERO;
        return;
    }
    if (!m_inertiaCollider || m_mass == 0.0f)
    {
        m_invInertiaDiag = math::Vector3::ZERO;
        return;
    }

    const float        m    = m_mass;
    const ColliderType type = m_inertiaCollider->GetType();

    if (type == ColliderType::SPHERE)
    {
        const float r   = static_cast<const SphereCollider*>(m_inertiaCollider)->m_radius;
        const float inv = 5.0f / (2.0f * m * r * r);
        m_invInertiaDiag = { inv, inv, inv };
    }
    else if (type == ColliderType::AABB)
    {
        // AABB は軸整合前提のため回転を許すと形状定義と衝突判定がずれる。
        m_invInertiaDiag = math::Vector3::ZERO;
        m_angularVelocity = math::Vector3::ZERO;
    }
    else if (type == ColliderType::CAPSULE)
    {
        const auto* capsule = static_cast<const CapsuleCollider*>(m_inertiaCollider);
        const float r = capsule->m_radius;
        const float h = capsule->m_halfHeight * 2.0f;
        const float ixz = (m * (3.0f * r * r + h * h)) / 12.0f;
        const float iy = 0.5f * m * r * r;
        m_invInertiaDiag = {
            ixz == 0.0f ? 0.0f : 1.0f / ixz,
            iy == 0.0f ? 0.0f : 1.0f / iy,
            ixz == 0.0f ? 0.0f : 1.0f / ixz
        };
    }
}

} // namespace fbzz::physics
