// FBZZ Engine
// NavigationSystem.hpp | fbzz::scene
// NavMeshAgentComponent を NavMesh 上で A* + Funnel Algorithm によりパス追従させる毎フレームシステム。
// PhysicsSystem の Transform 書き戻し (TransformSystem) の後、LateScriptSystem の前に呼ぶ。
#pragma once

namespace fbzz::scene {
class Scene;
void NavigationSystem(Scene& scene, float dt);
} // namespace fbzz::scene
