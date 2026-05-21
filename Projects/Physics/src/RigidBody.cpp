// FBZZ Engine
// RigidBody.cpp | fbzz::physics
// 剛体の状態と力の積分
#include <Physics/RigidBody.hpp>
#include <Physics/SphereCollider.hpp>
#include <Physics/AABBCollider.hpp>

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

void RigidBody::SetPosition(const math::Vector3& pos) { m_position = pos; }
void RigidBody::SetVelocity(const math::Vector3& vel) { m_velocity = vel; }

void RigidBody::SetRotation(const math::Quaternion& rot)
{
    m_rotation = rot.Normalized();
}

void RigidBody::SetCollider(std::shared_ptr<Collider> collider)
{
    if (m_collider) m_collider->m_body = nullptr;
    m_collider = std::move(collider);
    if (m_collider) m_collider->m_body = this;
    RecomputeInertia();
}

void RigidBody::Integrate(float dt)
{
    if (!m_isStatic)
    {
        // Linear (semi-implicit Euler)
        // m_force には World が事前に重力を ApplyForce 済み
        m_velocity += m_force * m_invMass * dt;
        m_position += m_velocity * dt;

        // Angular
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

    // static ボディ含め毎フレームリセット (蓄積防止)
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
    if (!m_collider || m_mass == 0.0f) return;

    const float        m    = m_mass;
    const ColliderType type = m_collider->GetType();

    if (type == ColliderType::SPHERE)
    {
        const float r   = static_cast<SphereCollider*>(m_collider.get())->m_radius;
        const float inv = 5.0f / (2.0f * m * r * r);
        m_invInertiaDiag = { inv, inv, inv };
    }
    else if (type == ColliderType::AABB)
    {
        const math::Vector3 h =
            static_cast<AABBCollider*>(m_collider.get())->m_halfExtents;
        m_invInertiaDiag = {
            3.0f / (m * (h.y * h.y + h.z * h.z)),
            3.0f / (m * (h.x * h.x + h.z * h.z)),
            3.0f / (m * (h.x * h.x + h.y * h.y))
        };
    }
    // CapsuleCollider は後回し実装時に追加
}

} // namespace fbzz::physics