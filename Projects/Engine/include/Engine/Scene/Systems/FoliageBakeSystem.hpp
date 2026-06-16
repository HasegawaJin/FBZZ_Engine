// FBZZ Engine
// FoliageBakeSystem.hpp | fbzz::scene
// FoliageComponent の STAMP 配置に合わせて子 GO 階層を管理するシステム。
// needsBake が true のタイミングで旧子 GO を破棄して再生成する。
// 各 stamp に対して CapsuleCollider + (SubMesh 数の) MeshRenderer 孫 GO を生成する。
// PhysicsSystem より前、TransformSystem の後に呼ぶ。
#pragma once

namespace fbzz::scene {
class Scene;
void FoliageBakeSystem(Scene& scene);
} // namespace fbzz::scene
