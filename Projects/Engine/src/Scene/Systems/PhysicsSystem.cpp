// FBZZ Engine
// PhysicsSystem.cpp | fbzz::scene
// physics::World を Step し、RigidBodyComponent の結果を Transform に書き戻す
#include "Engine/Scene/Systems/PhysicsSystem.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Components/RigidBodyComponent.hpp"
#include <Physics/World.hpp>

namespace fbzz::scene {

void PhysicsSystem(Scene& scene, physics::World& world, float dt) {
    world.Step(dt);

    for (auto [tf, rb] : scene.View<Transform, RigidBodyComponent>()) {
        if (!rb.enabled || !rb.rigidBody) continue;
        tf.position = rb.rigidBody->GetPosition();
        tf.rotation = rb.rigidBody->GetRotation();
    }
}

} // namespace fbzz::scene
