// FBZZ Engine
// NavMeshPatrolSystem.hpp | fbzz::scene
// NavMeshPatrolComponent を持つ GameObject の NavMeshAgentComponent へ、
// 巡回ウェイポイントの目的地を順番に SetDestination する。NavigationSystem の前に呼ぶ。
#pragma once

namespace fbzz::scene {
class Scene;
void NavMeshPatrolSystem(Scene& scene, float dt);
} // namespace fbzz::scene
