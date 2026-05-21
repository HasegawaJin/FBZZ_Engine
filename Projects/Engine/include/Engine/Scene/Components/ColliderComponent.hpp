// FBZZ Engine
// ColliderComponent.hpp | fbzz::scene
// Unity-style collider component driven by GameObject Transform
#pragma once
#include <Physics/Collider.hpp>
#include <Physics/PhysicsMaterial.hpp>
#include <memory>

namespace fbzz::scene {

struct ColliderComponent {
    std::shared_ptr<physics::Collider> collider;
    physics::PhysicsMaterial material = physics::PhysicsMaterial::Default;
    bool isTrigger = false;
    bool enabled = true;
};

} // namespace fbzz::scene
