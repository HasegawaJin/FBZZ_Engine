// FBZZ Engine
// PhysicsSystem.cpp | fbzz::scene
// physics::World を Step し、RigidBodyComponent の結果を Transform に書き戻す
#include "engine/Scene/Systems/PhysicsSystem.hpp"
#include "engine/Scene/Scene.hpp"
#include "engine/Scene/Components/RigidBodyComponent.hpp"
#include <physics/World.hpp>

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
