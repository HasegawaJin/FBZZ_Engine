// FBZZ Engine
// IKSystem.hpp | fbzz::scene
// AnimatorSystem が確定した FK ポーズに解析的 2-Bone IK を後処理として適用し、スキニング行列を再アップロードする。
#pragma once

namespace fbzz::renderer { class ResourceManager; }
namespace fbzz::physics  { class World; }

namespace fbzz::scene {

class Scene;

// WHY: 地面スナップレイキャストを World::Raycast に委譲するため World を受け取る。
//      重複していたローカル RaycastAABB/OBB/Triangle ヘルパーはこれにより不要になった。
void IKSystem(Scene& scene, physics::World& world,
              renderer::ResourceManager& resources, float dt);

} // namespace fbzz::scene
