// FBZZ Engine
// RigidBodyComponent.hpp | fbzz::scene
// physics::RigidBody を Scene に紐付けるコンポーネント
#pragma once
#include <physics/RigidBody.hpp>
#include <memory>

namespace fbzz::scene {

struct RigidBodyComponent {
    std::shared_ptr<physics::RigidBody> rigidBody;
    bool enabled = true;
};

} // namespace fbzz::scene
