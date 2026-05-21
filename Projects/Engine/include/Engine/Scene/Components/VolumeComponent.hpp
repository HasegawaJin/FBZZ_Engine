// FBZZ Engine
// VolumeComponent.hpp | fbzz::scene
// Trigger collider area effect component
#pragma once
#include <Math/Vector3.hpp>
#include <Physics/ColliderVolume.hpp>

namespace fbzz::scene {

struct VolumeComponent {
    physics::VolumeType type = physics::VolumeType::Gravity;
    bool enabled = true;

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
    float elapsed = 0.0f;
};

} // namespace fbzz::scene
