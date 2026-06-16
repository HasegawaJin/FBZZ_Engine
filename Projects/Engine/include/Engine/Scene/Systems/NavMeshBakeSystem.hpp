// FBZZ Engine
// NavMeshBakeSystem.hpp | fbzz::scene
// NavMeshVolumeComponent::needsBake が true のときに NavMesh を再構築するシステム。
// Editor の明示的な Bake 操作でのみ実行され、Runtime での自動再 Bake は行わない。
// PhysicsSystem より前 (TransformSystem の後) に呼ぶ。
#pragma once

namespace fbzz::scene {
class Scene;
void NavMeshBakeSystem(Scene& scene);
} // namespace fbzz::scene
