// FBZZ Engine
// ColliderVolume.cpp | fbzz::physics
// Trigger collider backed area effects
#include <Physics/ColliderVolume.hpp>
#include <Physics/SphereCollider.hpp>
#include <Physics/AABBCollider.hpp>
#include <Physics/CapsuleCollider.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::physics
{
    ColliderVolume::ColliderVolume(std::shared_ptr<Collider> collider, const VolumeSettings& settings)
        : m_collider(std::move(collider)), m_settings(settings)
    {
    }

    bool ColliderVolume::Contains(const math::Vector3& position) const
    {
        if (!m_collider) return false;

        if (m_collider->GetType() == ColliderType::SPHERE)
        {
            const auto* sphere = static_cast<const SphereCollider*>(m_collider.get());
            const math::Vector3 center = sphere->GetAABB().Center();
            return (position - center).LengthSq() <= sphere->m_radius * sphere->m_radius;
        }

        if (m_collider->GetType() == ColliderType::AABB)
        {
            const AABB aabb = m_collider->GetAABB();
            return position.x >= aabb.min.x && position.x <= aabb.max.x &&
                   position.y >= aabb.min.y && position.y <= aabb.max.y &&
                   position.z >= aabb.min.z && position.z <= aabb.max.z;
        }

        const auto* capsule = static_cast<const CapsuleCollider*>(m_collider.get());
        const math::Vector3 segment = capsule->GetSegmentEnd() - capsule->GetSegmentStart();
        const float lenSq = segment.LengthSq();
        float t = 0.0f;
        if (lenSq > 1e-6f)
            t = std::clamp(math::Vector3::Dot(position - capsule->GetSegmentStart(), segment) / lenSq, 0.0f, 1.0f);
        const math::Vector3 closest = capsule->GetSegmentStart() + segment * t;
        return (position - closest).LengthSq() <= capsule->m_radius * capsule->m_radius;
    }

    void ColliderVolume::Apply(RigidBody& body, float /*dt*/)
    {
        if (body.IsStatic() || !m_collider) return;

        const AABB aabb = m_collider->GetAABB();
        const math::Vector3 center = aabb.Center();

        switch (m_settings.type)
        {
        case VolumeType::Gravity:
            body.ApplyForce(m_settings.gravity * body.GetMass());
            break;
        case VolumeType::Vortex:
        {
            const math::Vector3 toCenter = center - body.GetPosition();
            const math::Vector3 flat = { toCenter.x, 0.0f, toCenter.z };
            const math::Vector3 inward = flat.LengthSq() > 1e-6f ? flat.Normalized() : math::Vector3::ZERO;
            const math::Vector3 tangent = { -inward.z, 0.0f, inward.x };
            body.ApplyForce((tangent * m_settings.swirlStrength +
                             inward * m_settings.inwardStrength +
                             math::Vector3::UP * m_settings.liftStrength) * body.GetMass());
            break;
        }
        case VolumeType::Buoyancy:
            body.ApplyForce(math::Vector3::UP * (m_settings.buoyancy * body.GetMass()));
            body.ApplyForce(-body.GetVelocity() * m_settings.drag);
            break;
        case VolumeType::Explosion:
        {
            const math::Vector3 delta = body.GetPosition() - center;
            const math::Vector3 dir = delta.LengthSq() > 1e-6f ? delta.Normalized() : math::Vector3::UP;
            body.ApplyImpulse(dir * m_settings.explosionImpulse);
            break;
        }
        case VolumeType::TimeDilation:
            break;
        case VolumeType::Magnetic:
            body.ApplyForce(math::Vector3::Cross(body.GetVelocity(), m_settings.magneticField) * body.m_charge);
            break;
        }
    }

    float ColliderVolume::GetTimeScale() const
    {
        return m_settings.type == VolumeType::TimeDilation ? m_settings.timeScale : 1.0f;
    }

    bool ColliderVolume::OverridesGravity() const
    {
        return m_settings.type == VolumeType::Gravity;
    }

    bool ColliderVolume::IsExpired() const
    {
        return m_settings.duration >= 0.0f && m_elapsed >= m_settings.duration;
    }

    void ColliderVolume::Tick(float dt)
    {
        m_elapsed += dt;
    }
} // namespace fbzz::physics
