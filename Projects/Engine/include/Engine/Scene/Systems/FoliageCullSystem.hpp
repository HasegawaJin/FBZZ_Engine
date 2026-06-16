// FBZZ Engine
// FoliageCullSystem.hpp | fbzz::scene
// STAMP 子 GO のコライダーをメインカメラ距離で有効/無効切り替えするシステム。
// FoliageBakeSystem と PhysicsSystem の間に呼ぶ。
#pragma once

namespace fbzz::scene {
class Scene;
void FoliageCullSystem(Scene& scene);
} // namespace fbzz::scene
