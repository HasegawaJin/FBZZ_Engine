// FBZZ Engine
// PhysicsSystem.hpp | fbzz::scene
// RigidBodyComponent ↔ physics::World を同期し、Transform に結果を書き戻す
#pragma once

namespace fbzz::scene   { class Scene; }
namespace fbzz::physics { class World; }

namespace fbzz::scene {

void PhysicsSystem(Scene& scene, physics::World& world, float dt);

} // namespace fbzz::scene
