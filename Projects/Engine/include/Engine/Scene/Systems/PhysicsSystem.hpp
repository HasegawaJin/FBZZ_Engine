// FBZZ Engine
// PhysicsSystem.hpp | fbzz::scene
// Scene と physics::World の同期 System
// RigidBodyComponent から World へ入力し、シミュレーション後の結果を Transform へ戻す。
// 依存方向は Scene から physics への一方向に保つ。
#pragma once

namespace fbzz::scene   { class Scene; }
namespace fbzz::physics { class World; }

namespace fbzz::scene {

void PhysicsSystem(Scene& scene, physics::World& world, float dt);

} // namespace fbzz::scene
