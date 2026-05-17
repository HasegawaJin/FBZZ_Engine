// FBZZ Engine
// TransformSystem.hpp | fbzz::scene
// 親子階層を走査し、world 空間の position / rotation を再計算する
#pragma once

namespace fbzz::scene {
class Scene;

void TransformSystem(Scene& scene);
} // namespace fbzz::scene
