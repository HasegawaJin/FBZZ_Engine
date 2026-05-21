// FBZZ Engine
// ColliderVolume.hpp | fbzz::physics
// Trigger collider backed area effects
#pragma once
#include <Physics/Volume.hpp>
#include <Physics/Collider.hpp>
#include <memory>

namespace fbzz::physics
{
    enum class VolumeType {
        Gravity,
        Vortex,
        Buoyancy,
        Explosion,
        TimeDilation,
        Magnetic
    };

    struct VolumeSettings {
        VolumeType type = VolumeType::Gravity;
        math::Vector3 gravity = { 0.0f, -9.81f, 0.0f };
        math::Vector3 magneticField = { 0.0f, 1.0f, 0.0f };
        float swirlStrength = 1.0f;
        float inwardStrength = 0.0f;
        float liftStrength = 0.0f;
        float buoyancy = 10.0f;
        float drag = 1.0f;
        float explosionImpulse = 10.0f;
        float timeScale = 1.0f;
        float duration = -1.0f;
    };

    class ColliderVolume : public Volume
    {
    public:
        ColliderVolume(std::shared_ptr<Collider> collider, const VolumeSettings& settings);

        bool Contains(const math::Vector3& position) const override;
        void Apply(RigidBody& body, float dt) override;
        float GetTimeScale() const override;
        bool OverridesGravity() const override;
        bool IsExpired() const override;

        void Tick(float dt) override;

    private:
        std::shared_ptr<Collider> m_collider;
        VolumeSettings m_settings;
        float m_elapsed = 0.0f;
    };
} // namespace fbzz::physics
