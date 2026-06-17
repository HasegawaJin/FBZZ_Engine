// FBZZ Engine
// NavMeshSensorSystem.hpp | fbzz::scene
// NavMeshSensorComponent の視野角・距離・遮蔽判定を毎フレーム評価し、
// autoChase が有効なら同 GO の NavMeshAgentComponent を自動で追跡させる。
// NavMeshPatrolSystem より前、NavigationSystem より前に呼ぶ
// (Patrol は target.IsValid() を見て巡回を中断するため)。
#pragma once

namespace fbzz::physics { class World; }

namespace fbzz::scene {
class Scene;
void NavMeshSensorSystem(Scene& scene, const physics::World* world, float dt);
} // namespace fbzz::scene
