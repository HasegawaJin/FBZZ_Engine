// FBZZ Engine
// RigidBody.cpp | fbzz::physics
// 剛体の状態と力の積分
#include <Physics/RigidBody.hpp>
#include <Physics/SphereCollider.hpp>
#include <Physics/AABBCollider.hpp>
#include <Physics/OBBCollider.hpp>
#include <Physics/CapsuleCollider.hpp>

namespace fbzz::physics {

namespace {

math::Vector3 ApplyAxisLock(const math::Vector3& value,
                            const AxisLock& lock,
                            const math::Vector3& fallback)
{
    return {
        lock.x ? fallback.x : value.x,
        lock.y ? fallback.y : value.y,
        lock.z ? fallback.z : value.z
    };
}

} // namespace

void RigidBody::ApplyForce(const math::Vector3& force)
{
    m_force += ApplyPositionFreeze(force, math::Vector3::ZERO);
}

void RigidBody::ApplyForceAtPoint(const math::Vector3& force,
                                   const math::Vector3& worldPoint)
{
    m_force  += ApplyPositionFreeze(force, math::Vector3::ZERO);
    m_torque += math::Vector3::Cross(worldPoint - m_position, force);
    m_torque = ApplyRotationFreeze(m_torque);
}

void RigidBody::ApplyImpulse(const math::Vector3& impulse)
{
    m_velocity = ApplyPositionFreeze(m_velocity + impulse * m_invMass, math::Vector3::ZERO);
}

void RigidBody::ApplyAngularImpulse(const math::Vector3& angularImpulse)
{
    m_angularVelocity = ApplyRotationFreeze(m_angularVelocity + ApplyInvInertia(angularImpulse));
}

void RigidBody::ApplyTorque(const math::Vector3& torque)
{
    m_torque += ApplyRotationFreeze(torque);
}

void RigidBody::SetMass(float mass)
{
    m_mass    = mass;
    m_invMass = (m_isStatic || mass == 0.0f) ? 0.0f : 1.0f / mass;
    RecomputeInertia();
}

void RigidBody::SetPosition(const math::Vector3& pos)
{
    m_position = ApplyPositionFreeze(pos, m_position);
}

void RigidBody::SetVelocity(const math::Vector3& vel)
{
    m_velocity = ApplyPositionFreeze(vel, math::Vector3::ZERO);
}

void RigidBody::SetAngularVelocity(const math::Vector3& v)
{
    m_angularVelocity = ApplyRotationFreeze(v);
}

void RigidBody::SetRotation(const math::Quaternion& rot)
{
    m_rotation = rot.Normalized();
}

void RigidBody::SetFreezePosition(const AxisLock& lock)
{
    m_freezePosition = lock;
    m_velocity = ApplyPositionFreeze(m_velocity, math::Vector3::ZERO);
    m_force = ApplyPositionFreeze(m_force, math::Vector3::ZERO);
}

void RigidBody::SetFreezeRotation(const AxisLock& lock)
{
    m_freezeRotation = lock;
    m_angularVelocity = ApplyRotationFreeze(m_angularVelocity);
    m_torque = ApplyRotationFreeze(m_torque);
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
        m_velocity = ApplyPositionFreeze(m_velocity, math::Vector3::ZERO);
        m_position = ApplyPositionFreeze(m_position + m_velocity * dt, m_position);

        // 角運動はボディ空間の対角慣性テンソルで角加速度を求め、ワールド空間へ戻す。
        math::Vector3 tauBody   = m_rotation.Conjugate() * m_torque;
        math::Vector3 alphaBody = {
            m_invInertiaDiag.x * tauBody.x,
            m_invInertiaDiag.y * tauBody.y,
            m_invInertiaDiag.z * tauBody.z
        };
        m_angularVelocity += (m_rotation * alphaBody) * dt;
        m_angularVelocity = ApplyRotationFreeze(m_angularVelocity);

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
    return ApplyRotationFreeze(m_rotation * scaled);
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
    else if (type == ColliderType::OBB)
    {
        const auto* box = static_cast<const OBBCollider*>(m_inertiaCollider);
        const float hx = box->m_halfExtents.x;
        const float hy = box->m_halfExtents.y;
        const float hz = box->m_halfExtents.z;
        const float ix = (m * (hy * hy + hz * hz)) / 3.0f;
        const float iy = (m * (hx * hx + hz * hz)) / 3.0f;
        const float iz = (m * (hx * hx + hy * hy)) / 3.0f;
        m_invInertiaDiag = {
            ix == 0.0f ? 0.0f : 1.0f / ix,
            iy == 0.0f ? 0.0f : 1.0f / iy,
            iz == 0.0f ? 0.0f : 1.0f / iz
        };
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

math::Vector3 RigidBody::ApplyPositionFreeze(const math::Vector3& value,
                                             const math::Vector3& base) const
{
    return ApplyAxisLock(value, m_freezePosition, base);
}

math::Vector3 RigidBody::ApplyRotationFreeze(const math::Vector3& value) const
{
    return ApplyAxisLock(value, m_freezeRotation, math::Vector3::ZERO);
}

} // namespace fbzz::physics
